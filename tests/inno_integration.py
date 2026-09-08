"""用官方 ISCC 生成惰性测试包；只执行编译器与 Extract，不执行安装器。

Python 仅用于开发测试，不进入便携发行目录。每次运行保留独立目录便于复查。
"""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import uuid
import zlib


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def crc(data):
    return struct.pack('<I', zlib.crc32(data))


class Package:
    """仅用于构造变异样本的独立测试读写器，不是产品运行依赖。"""
    def __init__(self, path):
        self.raw = bytearray(path.read_bytes())
        self.table = self.raw.index(b'rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a')
        self.legacy = struct.unpack_from('<I', self.raw, self.table + 12)[0] == 1
        self.loader_crc = self.table + (40 if self.legacy else 60)
        self.metadata, self.payload = struct.unpack_from('<II' if self.legacy else '<QQ', self.raw, self.table + (32 if self.legacy else 40))
        self.prefix = self.raw[:self.metadata + (64 if self.legacy else 117)]
        self.data_id = self.raw[self.metadata:self.metadata + 64].rstrip(b'\0').decode('ascii')
        self.sha1 = any(f'({v})' in self.data_id for v in ('6.0.0', '6.1.0', '6.3.0'))
        self.old_flags = self.sha1 or any(f'({v})' in self.data_id for v in ('6.4.0.1', '6.4.2'))
        self.sign_byte = self.old_flags and '(6.0.0)' not in self.data_id and '(6.1.0)' not in self.data_id
        self.version7 = b'(7.' in self.raw[self.metadata:self.metadata + 64]
        self.version650 = b'(6.5.0)' in self.raw[self.metadata:self.metadata + 64]
        self.short_offset = self.legacy or self.version650
        self.wide_size = self.version7 or b'(6.7.' in self.raw[self.metadata:self.metadata + 64]
        self.block_start = len(self.prefix)
        self.location_size = (74 if self.sha1 else 86) + int(self.sign_byte) if self.old_flags else (85 if self.short_offset else 89)
        self.size_offset = 20 if self.short_offset else 24
        self.packed_offset = 28 if self.short_offset else 32
        self.hash_offset = 36 if self.short_offset else 40
        self.flag_offset = self.hash_offset + (20 if self.sha1 else 32) + 16
        self.compressed_flag = 128 if self.old_flags else 16
        self.encrypted_flag = 64 if self.old_flags else 8
        position = self.block_start
        self.blocks = []
        for _ in range(2):
            width = 13 if self.wide_size else 9
            length, compressed = struct.unpack_from('<QB' if self.wide_size else '<IB', self.raw, position + 4)
            check(crc(self.raw[position + 4:position + width]) == self.raw[position:position + 4], 'fixture header CRC')
            position += width
            end = position + length
            joined = bytearray()
            while position < end:
                expected = self.raw[position:position + 4]
                position += 4
                chunk = self.raw[position:min(end, position + 4096)]
                check(crc(chunk) == expected, 'fixture block CRC')
                joined.extend(chunk)
                position += len(chunk)
            if compressed:
                prop = joined[0]
                filters = [{'id': lzma.FILTER_LZMA1, 'lc': prop % 9, 'lp': (prop // 9) % 5,
                            'pb': prop // 45, 'dict_size': struct.unpack_from('<I', joined, 1)[0]}]
                joined = lzma.decompress(joined[5:], format=lzma.FORMAT_RAW, filters=filters)
            self.blocks.append(bytearray(joined))

    def rebuild(self):
        data = bytearray(self.prefix)
        for block in self.blocks:
            framed = b''.join(crc(block[i:i + 4096]) + block[i:i + 4096] for i in range(0, len(block), 4096))
            header = struct.pack('<QB' if self.wide_size else '<IB', len(framed), 0)
            data += crc(header) + header + framed
        struct.pack_into('<I' if self.legacy else '<Q', data, self.table + 16, len(data))
        data[self.loader_crc:self.loader_crc + 4] = crc(data[self.table:self.loader_crc])
        return data


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--work-root', required=True, type=Path)
    parser.add_argument('--compiler', type=Path)
    args = parser.parse_args()
    compiler = args.compiler or Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Inno Setup 6/ISCC.exe'
    if not compiler.is_file():
        print('SKIP: no official Inno compiler; specify --compiler ISCC.exe')
        return 77
    compiler = compiler.resolve()
    executable = args.executable.resolve()
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    print(f'Fixtures: {root}', flush=True)
    probe = root / 'compiler-probe.iss'
    probe.write_text('[Setup]\nAppName=Compiler Probe\nAppVersion=1\nDefaultDirName={autopf}\\Probe\nOutput=no\n', encoding='utf-8-sig')
    banner_result = subprocess.run([str(compiler), str(probe)], capture_output=True, timeout=30)
    banner = (banner_result.stdout + banner_result.stderr).decode('utf-8', errors='replace')
    match = re.search(r'Compiler engine version: Inno Setup (\d+)\.(\d+)\.(\d+)', banner)
    check(match is not None, 'ISCC version missing')
    version = tuple(map(int, match.groups()))
    check(version >= (6, 0, 5), f'unsupported fixture compiler {version}')
    version7 = version[0] == 7
    print(f'Official compiler: {compiler} ({".".join(match.groups())})', flush=True)
    raw = bytearray(random.Random(321).randbytes(150_007))
    # 有意跨越多个 64 KiB 分段，并含正负 CALL/JMP 操作数；不是可执行文件。
    for offset in (0, 17, 65531, 65534, 65536, 131069, 131072, 149998):
        raw[offset:offset + 5] = b'\xe8\x44\x23\x01\x00'
        raw[offset + 5:offset + 10] = b'\xe9\x55\x34\x82\xff'
    original = {'payload.dat': bytes(raw), 'empty.txt': b'', '中文 😀.txt': '提取基准\n'.encode(),
                'variant-a.txt': b'Condition A\n', 'variant-b.txt': b'Condition B\n'}
    for name, data in original.items():
        (root / name).write_bytes(data)
    run_count = 0

    def run(arguments, expected=0):
        nonlocal run_count
        run_count += 1
        result = subprocess.run([str(executable), '--quiet', *map(str, arguments)], capture_output=True, timeout=30)
        stdout = result.stdout.decode('utf-8', errors='strict')
        stderr = result.stderr.decode('utf-8', errors='strict')
        check(result.returncode == expected, f'{arguments}: expected {expected}, got {result.returncode}\n{stdout}\n{stderr}')
        return stdout

    def build(name, method='lzma2', solid=True, architecture='x86', setup='', files=None, code=''):
        if files is None:
            files = '\n'.join([
                'Source: "empty.txt"; DestDir: "{app}"',
                'Source: "payload.dat"; DestDir: "{app}\\bin"',
                'Source: "payload.dat"; DestDir: "{app}\\alias"; DestName: "copy.dat"',
                'Source: "中文 😀.txt"; DestDir: "{app}\\中文 目录"',
                'Source: "variant-a.txt"; DestDir: "{app}"; DestName: "choice.txt"; Components: one; Tasks: optional',
                'Source: "variant-b.txt"; DestDir: "{app}"; DestName: "choice.txt"; Components: two',
                'Source: "variant-a.txt"; DestDir: "{app}"; DestName: "prefix"',
                'Source: "variant-b.txt"; DestDir: "{app}\\prefix"; DestName: "child.txt"',
                'Source: "empty.txt"; DestDir: "{app}\\last"; Flags: solidbreak',
                'Source: "payload.dat"; DestDir: "{app}\\last"; DestName: "filtered.dat"; Flags: nocompression',
            ])
        architecture_setting = f'SetupArchitecture={architecture}' if version7 else ''
        script = f'''[Setup]
AppName=Extract Inert Fixture
AppVersion=1
DefaultDirName={{autopf}}\\Extract Fixture
OutputDir=.
OutputBaseFilename={name}
Compression={method}
SolidCompression={'yes' if solid else 'no'}
LZMAUseSeparateProcess=no
{architecture_setting}
{setup}
[Types]
Name: full; Description: "Full"
[Components]
Name: one; Description: "One"; Types: full
Name: two; Description: "Two"; Types: full
[Tasks]
Name: optional; Description: "Optional"
[Dirs]
Name: "{{app}}\\empty directory"
[CustomMessages]
Test=Known custom message
[Files]
{files}
[Code]
{code}
'''
        iss = root / f'{name}.iss'
        iss.write_text(script, encoding='utf-8-sig')
        result = subprocess.run([str(compiler), '/Q', str(iss)], cwd=root, capture_output=True, timeout=90)
        check(result.returncode == 0, f'ISCC {name}: {result.stdout!r}\n{result.stderr!r}')
        return root / f'{name}.exe'

    def verify(package, expected=None, partial=False):
        before = sha(package.read_bytes())
        before_dirs = set(root.glob('*_extracted*'))
        listed = json.loads(run(['--list', package]))
        check(set(root.glob('*_extracted*')) == before_dirs, '--list created output')
        check(listed['status'] == 'listed', 'list status')
        run([package], 299 if partial else 0)
        new_dirs = set(root.glob('*_extracted*')) - before_dirs
        check(len(new_dirs) == 1, 'output directory count')
        output = new_dirs.pop()
        report = json.loads((output / '_extract-report.json').read_text(encoding='utf-8'))
        check(report['status'] == ('partial' if partial else 'complete'), 'report status')
        check(report['format'] == 'Inno Setup', 'format')
        check(sha(package.read_bytes()) == before, 'input changed')
        check(len(report['files']) == len(listed['files']), 'list/extract disagreement')
        expected = expected or {
            '{app}\\empty.txt': b'', '{app}\\bin\\payload.dat': original['payload.dat'],
            '{app}\\alias\\copy.dat': original['payload.dat'], '{app}\\中文 目录\\中文 😀.txt': original['中文 😀.txt'],
            '{app}\\choice.txt|Components=one': original['variant-a.txt'],
            '{app}\\choice.txt|Components=two': original['variant-b.txt'],
            '{app}\\prefix': original['variant-a.txt'], '{app}\\prefix\\child.txt': original['variant-b.txt'],
            '{app}\\last\\empty.txt': b'', '{app}\\last\\filtered.dat': original['payload.dat'],
        }
        check(len(report['files']) == len(expected), 'missing/extra files')
        seen = set()
        for entry in report['files']:
            key = entry['sourceExpression']
            if key == '{app}\\choice.txt':
                key += '|' + entry['conditions'].split(';')[0]
            check(key in expected and key not in seen, f'unknown/duplicate entry {key}')
            seen.add(key)
            data = (output / entry['path']).read_bytes()
            check(data == expected[key], f'wrong bytes {entry}')
            check(entry['sourceHashVerified'] and entry['sha256'] == sha(data), f'hash not verified {entry}')
            check(entry['sourceHashAlgorithm'] == ('SHA-1' if version < (6, 4, 0) else 'SHA-256'), 'wrong source hash algorithm')
            if entry['sourceExpression'] == '{app}\\bin\\payload.dat':
                check(entry['path'] == 'app/bin/payload.dat', 'logical app path')
            if 'choice.txt' in key or key in ('{app}\\prefix', '{app}\\prefix\\child.txt'):
                check(entry['path'].startswith('_variants/'), f'collision was not preserved {entry}')
            if partial:
                check(entry['path'].startswith('_unresolved/'), 'dynamic path output')
        check(len(list(output.rglob('*'))) >= len(expected) + 1, 'missing output')
        return report

    built = {}
    for architecture in (('x86', 'x64') if version7 else ('x86',)):
        for method in ('none', 'zip', 'bzip', 'lzma', 'lzma2'):
            for solid in (False, True):
                name = f'{architecture}-{method}-{"solid" if solid else "blocks"}'
                package = build(name, method, solid, architecture)
                verify(package)
                built[name] = package
                print(f'PASS {name}', flush=True)
    base = built['x86-lzma2-solid']
    verify(base)  # 重复运行不得覆盖已完成输出。
    plain_meta = build('stored-metadata', setup='InternalCompressLevel=none')
    verify(plain_meta)
    dynamic = build('dynamic', files='Source: "payload.dat"; DestDir: "{code:GetDir}"',
                    code="function GetDir(Param: String): String; begin Result := ExpandConstant('{app}'); end;")
    verify(dynamic, {'{code:GetDir}\\payload.dat': original['payload.dat']}, partial=True)
    embedded_only = build('dontcopy', files='Source: "payload.dat"; Flags: dontcopy')
    verify(embedded_only, {'{tmp}\\payload.dat': original['payload.dat']})
    external = build('external', files='Source: "C:\\missing\\external.dat"; DestDir: "{app}"; ExternalSize: 1; Flags: external skipifsourcedoesntexist')
    if version < (6, 4, 0):
        # 旧官方编译器的可选 ISCrypt.dll 不随测试工具分发。直接验证密码 UI 与加密标志的区别。
        password_only = build('password-only', setup='Password=fixture-secret\nEncryption=no')
        verify(password_only)
        modified = Package(password_only)
        cursor = 0
        for _ in range(34 if version < (6, 3, 0) else 36):
            cursor += 4 + struct.unpack_from('<I', modified.blocks[0], cursor)[0]
        modified.blocks[0][cursor + (161 if version < (6, 3, 0) else 159) + 4] |= 0x10
        encrypted = root / 'encrypted-marker.exe'
        encrypted.write_bytes(modified.rebuild())
    else:
        encrypted = build('encrypted', setup='Password=fixture-secret\nEncryption=yes')

    invalid_count = 0

    def reject(path, expected=13):
        nonlocal invalid_count
        before = set(root.iterdir())
        run([path], expected)
        check(set(root.iterdir()) == before, f'failed job left output/staging: {path}')
        invalid_count += 1

    reject(external, 50)
    reject(encrypted, 50)

    def mutate(name, change, expected=13, raw=False):
        sample = Package(base)
        if raw:
            data = bytearray(sample.raw)
            change(data, sample)
        else:
            change(sample)
            data = sample.rebuild()
        target = root / f'bad-{name}.exe'
        target.write_bytes(data)
        reject(target, expected)

    mutate('loader-crc', lambda d, p: d.__setitem__(p.loader_crc, d[p.loader_crc] ^ 1), raw=True)
    mutate('metadata-header-crc', lambda d, p: d.__setitem__(p.block_start, d[p.block_start] ^ 1), raw=True)
    mutate('metadata-content-crc', lambda d, p: d.__setitem__(p.block_start + 13, d[p.block_start + 13] ^ 1), raw=True)
    if version >= (6, 5, 0):
        mutate('encryption-crc', lambda d, p: d.__setitem__(p.metadata + 64, d[p.metadata + 64] ^ 1), raw=True)
    mutate('unknown-version', lambda d, p: d.__setitem__(slice(p.metadata, p.metadata + 64), b'Inno Setup Setup Data (99.0)'.ljust(64, b'\0')), 50, True)
    mutate('truncated', lambda d, p: d.__delitem__(slice(-200, None)), raw=True)
    mutate('string-length', lambda p: struct.pack_into('<I', p.blocks[0], 0, 0xfffffff0))
    mutate('odd-utf16', lambda p: struct.pack_into('<I', p.blocks[0], 0, 3))
    mutate('location-length', lambda p: p.blocks[1].pop())
    mutate('file-hash', lambda p: p.blocks[1].__setitem__(p.hash_offset, p.blocks[1][p.hash_offset] ^ 1))
    mutate('output-integer-overflow', lambda p: struct.pack_into('<Q', p.blocks[1], p.size_offset, 1 << 63), 223)
    mutate('chunk-offset', lambda p: struct.pack_into('<I' if p.short_offset else '<Q', p.blocks[1], 8, 2**32 - 1 if p.short_offset else 2**64 - 1))
    # Inline payload with FirstSlice > LastSlice is corrupt, not an external set.
    mutate('external-volume', lambda p: struct.pack_into('<I', p.blocks[1], 0, 1), 13)
    mutate('encrypted-location', lambda p: p.blocks[1].__setitem__(p.flag_offset, p.blocks[1][p.flag_offset] | p.encrypted_flag), 50)
    if version < (6, 4, 0):
        def unsupported_header(p):
            cursor = 0
            for _ in range(34 if version < (6, 3, 0) else 36):
                cursor += 4 + struct.unpack_from('<I', p.blocks[0], cursor)[0]
            # 修改版可能沿用标准版本标识；不得将不匹配的头部当成正常数据。
            struct.pack_into('<I', p.blocks[0], cursor + 64 + 74, 256)
        mutate('unsupported-header-layout', unsupported_header, 50)
    if Package(base).sign_byte:
        mutate('unknown-sign-mode', lambda p: p.blocks[1].__setitem__(p.flag_offset + 2, 255))

    def replace_path(sample, before, after):
        old = before.encode('utf-16le')
        new = after.encode('utf-16le')
        check(len(old) == len(new), 'test path replacement length')
        position = sample.blocks[0].index(old)
        sample.blocks[0][position:position + len(old)] = new

    mutate('traversal', lambda p: replace_path(p, '{app}\\bin\\payload.dat', '{app}\\..\\.payload.dat'), 5)
    mutate('ads', lambda p: replace_path(p, '{app}\\bin\\payload.dat', '{app}\\bin\\pay:oad.dat'), 5)
    mutate('device', lambda p: replace_path(p, '{app}\\bin\\payload.dat', '{app}\\bin\\CON    .dat'), 5)

    def dictionary(sample):
        records = sample.blocks[1]
        for index in range(0, len(records), sample.location_size):
            start = struct.unpack_from('<I' if sample.short_offset else '<Q', records, index + 8)[0]
            if records[index + sample.flag_offset] & sample.compressed_flag:
                sample.prefix[sample.payload + start + 4] = 40
                return
        raise AssertionError('no LZMA2 stream')

    mutate('dictionary-budget', dictionary, 223)
    mutate('pe-offset', lambda d, p: struct.pack_into('<I', d, 60, 0xfffffff0), raw=True)
    mutate('pe-section-count', lambda d, p: struct.pack_into('<H', d, struct.unpack_from('<I', d, 60)[0] + 6, 65535), raw=True)
    def unknown_flags(p):
        offset = p.flag_offset + (1 if p.old_flags else 0)
        p.blocks[1][offset] |= 128
    mutate('unknown-location-flags', unknown_flags)
    for name, package in built.items():
        if not name.startswith('x86-'):
            continue
        sample = Package(package)
        records = sample.blocks[1]
        first = next(i for i in range(0, len(records), sample.location_size) if struct.unpack_from('<Q', records, i + sample.size_offset)[0] > 0)
        start_format = '<I' if sample.short_offset else '<Q'
        start, = struct.unpack_from(start_format, records, first + 8)
        packed, = struct.unpack_from('<Q', records, first + sample.packed_offset)
        for i in range(0, len(records), sample.location_size):
            if struct.unpack_from(start_format, records, i + 8)[0] == start:
                struct.pack_into('<Q', records, i + sample.packed_offset, packed - 1)
        target = root / f'bad-truncated-stream-{name}.exe'
        target.write_bytes(sample.rebuild())
        reject(target)
    # 混合格式入口可以继续处理后一包，失败不能吞掉成功结果。
    run([root / 'bad-loader-crc.exe', base], 299)
    evidence = {'compiler': str(compiler), 'version': list(version), 'matrixPackages': len(built),
                'invalidCases': invalid_count, 'extractInvocations': run_count, 'fixtures': str(root)}
    (root / 'validation.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(evidence, ensure_ascii=False), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
