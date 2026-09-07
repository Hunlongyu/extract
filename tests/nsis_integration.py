"""Official makensis fixtures; execute the compiler and Extract only, never installers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
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


class Package:
    """Independent test mutator for an official, uncompressed NSIS fixture."""
    def __init__(self, path):
        self.raw = bytearray(path.read_bytes())
        self.start = self.raw.index(b'\xef\xbe\xad\xdeNullsoftInst') - 4
        self.header_size = struct.unpack_from('<I', self.raw, self.start + 20)[0]
        self.header_start = self.start + 32
        check(struct.unpack_from('<I', self.raw, self.start + 28)[0] == self.header_size, 'stored header required')
        self.header = bytearray(self.raw[self.header_start:self.header_start + self.header_size])
        self.data = bytearray(self.raw[self.header_start + self.header_size:-4])
        self.entries, self.count = struct.unpack_from('<II', self.header, 20)
        self.strings = struct.unpack_from('<I', self.header, 28)[0]
        self.file_entries = [self.entries + i * 28 for i in range(self.count)
                             if struct.unpack_from('<I', self.header, self.entries + i * 28)[0] == 20]

    def rebuild(self):
        data = self.raw[:self.header_start] + self.header + self.data
        struct.pack_into('<I', data, self.start + 24, len(data) + 4 - self.start)
        return data + struct.pack('<I', zlib.crc32(data[512:]))

    def rename(self, before, after):
        before, after = before.encode('utf-16le'), after.encode('utf-16le')
        check(len(before) == len(after), 'mutation replacement length')
        pos = self.header.index(before, self.strings)
        self.header[pos:pos + len(before)] = after


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    parser.add_argument('--compiler', type=Path)
    args = parser.parse_args()
    compiler = args.compiler or Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'NSIS/Bin/makensis.exe'
    if not compiler.is_file():
        print('SKIP: specify --compiler with a verified official makensis.exe')
        return 77
    compiler, executable = compiler.resolve(), args.executable.resolve()
    version = subprocess.run([str(compiler), '/VERSION'], capture_output=True, timeout=15).stdout.decode().strip()
    check(version.startswith('v3.'), f'unsupported compiler: {version}')
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    print(f'Fixtures: {root}', flush=True)
    cache = root / 'temporary-cache'
    cache.mkdir()
    environment = dict(os.environ, TEMP=str(cache), TMP=str(cache))
    payload = bytes(range(256)) * 4100 + b'\xe8\xff\xff\xe9\x00' * 27000
    noise = random.Random(319).randbytes(150003)
    sources = {'payload.bin': payload, 'noise.bin': noise, 'empty.dat': b'',
               'hello.txt': '中文路径\nUnicode 😀\n'.encode(), 'other.dat': b'other version\n'}
    for name, data in sources.items():
        (root / name).write_bytes(data)
    expected = {'app/bin/payload.bin': payload, 'app/noise.bin': noise, 'app/empty.dat': b'',
                'app/中文/说明.txt': sources['hello.txt'], 'temp/alias.bin': payload}
    run_count = 0

    def run(arguments, code=0):
        nonlocal run_count
        result = subprocess.run([str(executable), '--quiet', *map(str, arguments)], capture_output=True, timeout=90, env=environment)
        run_count += 1
        check(not list(cache.iterdir()), 'NSIS cache not cleaned after process exit')
        check(result.returncode == code, f'{arguments}: expected {code}, got {result.returncode}: {result.stderr.decode("utf-8", errors="replace")}')
        return result.stdout.decode('utf-8')

    def build(name, method='zlib', solid=False, stored=False, body=None, extra='', unicode=True):
        body = body or '''SetOutPath "$INSTDIR\\bin"
File "payload.bin"
SetOutPath "$INSTDIR"
File "noise.bin"
File "empty.dat"
SetOutPath "$INSTDIR\\中文"
File /oname=说明.txt "hello.txt"
SetOutPath "$TEMP"
File /oname=alias.bin "payload.bin"
'''
        script = (f'Unicode {"true" if unicode else "false"}\nName "Extract NSIS fixture"\n'
                  f'OutFile "{name}.exe"\nSetCompressor {"/SOLID " if solid else ""}{method}\n'
                  + ('SetCompress off\n' if stored else '') + extra + '\nSection\n' + body + '\nSectionEnd\n')
        path = root / (name + '.nsi')
        path.write_text(script, encoding='utf-8-sig')
        result = subprocess.run([str(compiler), '/V2', str(path)], capture_output=True, timeout=60)
        check(result.returncode == 0, f'compile {name}: {result.stdout!r} {result.stderr!r}')
        return root / (name + '.exe')

    def extract(path, expected_files, code=0, crc=True):
        listing = json.loads(run(['--list', path]))
        check(listing['format'] == 'NSIS', 'format')
        check(listing['packageCrcVerified'] == crc, 'CRC report')
        check(all(not f['sha256'] and not f['sourceHashVerified'] for f in listing['files']), 'list must not claim per-file verification')
        target = root / ('results-' + uuid.uuid4().hex)
        target.mkdir()
        run(['--output', target, path], code)
        results = list(target.iterdir())
        check(len(results) == 1 and '.tmp' not in results[0].name, 'output commit')
        out = results[0]
        report = json.loads((out / '_extract-report.json').read_text(encoding='utf-8'))
        check(report['status'] == ('complete' if code == 0 else 'partial'), 'completion report')
        observed = {f['path']: (out / f['path']).read_bytes() for f in report['files']}
        if expected_files is not None:
            check(observed == expected_files, f'payload mismatch: {path}, {list(observed)}')
        check(sum(f['size'] for f in report['files']) == report['totalBytes'], 'size total')
        for f in report['files']:
            check(f['sha256'] == sha(observed[f['path']]), 'output SHA-256')
            check(f['size'] == len(observed[f['path']]), 'file size')
            check(not f['sourceHashVerified'] and not f['sourceHashAlgorithm'], 'NSIS does not carry per-file hash')
        return report, observed

    built = []
    for method in ('zlib', 'bzip2', 'lzma'):
        for solid in (False, True):
            path = build(method + ('-solid' if solid else ''), method, solid)
            extract(path, expected)
            built.append(path)
    base = build('stored', stored=True)
    extract(base, expected)
    odd_dictionary = build('lzma-dictionary-3mb', method='lzma', extra='SetCompressorDictSize 3')
    extract(odd_dictionary, expected)
    no_crc = build('no-crc', extra='CRCCheck off')
    extract(no_crc, expected, crc=False)
    custom_brand = build('custom-brand', extra='BrandingText "Fixture Custom Brand"')
    report, _ = extract(custom_brand, expected)
    check('custom branding' in report['formatVersion'], 'custom branding must not invent an exact version')
    # Multiple instructions sharing one payload, conflicting destinations and directory prefixes.
    variants = build('variants', body='''SetOutPath "$INSTDIR"
File /oname=same.dat "hello.txt"
File /oname=same.dat "other.dat"
File /oname=same.dat "hello.txt"
File /oname=node "hello.txt"
SetOutPath "$INSTDIR\\node"
File "other.dat"
''')
    report, files = extract(variants, None)
    check(len(files) == 4 and all(p.startswith('_variants/') for p in files), 'conflict preservation / exact duplicate coalescing')
    check(sorted(files.values()) == sorted([sources['hello.txt']] * 2 + [sources['other.dat']] * 2), 'variant content')
    # Inert PE headers label architectures; none of these payloads are executed.
    def pe_payload(machine, marker):
        data = bytearray(1024)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 128)
        data[128:132] = b'PE\0\0'
        struct.pack_into('<H', data, 132, machine)
        struct.pack_into('<H', data, 148, 224 if machine == 0x14c else 240)
        struct.pack_into('<H', data, 152, 0x10b if machine == 0x14c else 0x20b)
        data[512:512 + len(marker)] = marker
        return bytes(data)

    native = {'x64.bin': pe_payload(0x8664, b'x64 program'),
              'x86.bin': pe_payload(0x14c, b'x86 program'),
              'arm64.bin': pe_payload(0xaa64, b'arm64 program'),
              'resource-a.bin': pe_payload(0x14c, b'resource for first branch'),
              'resource-b.bin': pe_payload(0x14c, b'resource for second branch')}
    for name, data in native.items():
        (root / name).write_bytes(data)
    architecture_body = '''SetOutPath "$INSTDIR"
File /oname=before.txt "hello.txt"
File /oname=helper.exe "x86.bin"
StrCmp $0 "64" native_a native_b
native_a:
SetOutPath "$INSTDIR"
File /oname=Main.exe "x64.bin"
File /oname=Main.tr "resource-a.bin"
File /oname=shared.txt "hello.txt"
File /oname=only64.txt "other.dat"
SetOutPath "$INSTDIR\\lib"
File /oname=engine.dll "x64.bin"
Goto joined
native_b:
SetOutPath "$INSTDIR"
File /oname=Main.exe "x86.bin"
File /oname=Main.tr "resource-b.bin"
File /oname=shared.txt "hello.txt"
SetOutPath "$INSTDIR\\lib"
File /oname=engine.dll "x86.bin"
joined:
SetOutPath "$INSTDIR"
File /oname=after.txt "other.dat"
SetOutPath "$TEMP"
File /oname=outside.txt "hello.txt"
'''
    layouts = {'temp/outside.txt': sources['hello.txt']}
    for arch in ('x64', 'x86'):
        prefix = 'app-' + arch + '/'
        layouts.update({prefix + 'before.txt': sources['hello.txt'], prefix + 'after.txt': sources['other.dat'],
                        prefix + 'helper.exe': native['x86.bin'], prefix + 'shared.txt': sources['hello.txt'],
                        prefix + 'Main.exe': native[arch + '.bin'], prefix + 'lib/engine.dll': native[arch + '.bin'],
                        prefix + 'Main.tr': native['resource-a.bin' if arch == 'x64' else 'resource-b.bin']})
    layouts['app-x64/only64.txt'] = sources['other.dat']
    for solid in (False, True):
        path = build('architectures-' + str(solid), method='lzma', solid=solid, body=architecture_body)
        report, _ = extract(path, layouts)
        check(len({f['id'] for f in report['files']}) == len(report['files']), 'layout IDs must remain unique')
        check(any('app-x64' in note and 'app-x86' in note for note in report['notes']), 'layout evidence missing')
        check(all(f['originalPath'].startswith('app/') for f in report['files'] if f['path'].startswith('app-')), 'original destinations lost')
    arm_layouts = {p.replace('app-x64/', 'app-arm64/'): (native['arm64.bin'] if p.startswith('app-x64/') and p.endswith(('Main.exe', 'engine.dll')) else data)
                   for p, data in layouts.items()}
    extract(build('architectures-arm64', body=architecture_body.replace('"x64.bin"', '"arm64.bin"')), arm_layouts)
    # Same-architecture alternatives, nested control flow, external entry, and conflicting
    # common files must retain the conservative per-file conflict layout.
    fallback_bodies = {
        'same-architecture': architecture_body.replace('"x64.bin"', '"x86.bin"'),
        'nested-control': architecture_body.replace('File /oname=only64.txt', 'IfFileExists "$TEMP\\flag" +2 0\nFile /oname=only64.txt'),
        'external-entry': 'Goto inside_a\n' + architecture_body.replace('File /oname=only64.txt', 'inside_a:\nFile /oname=only64.txt'),
        'common-conflict': architecture_body.replace('File /oname=before.txt "hello.txt"', 'File /oname=Main.tr "noise.bin"'),
        'mixed-machine': architecture_body.replace('File /oname=engine.dll "x64.bin"', 'File /oname=engine.dll "arm64.bin"'),
    }
    for name, body in fallback_bodies.items():
        _, files = extract(build(name, body=body), None, 299 if name == 'external-entry' else 0)
        check(not any(p.startswith(('app-x64/', 'app-x86/', 'app-arm64/')) for p in files), 'ambiguous layout was guessed: ' + name)
        check(any(p.startswith('_variants/') for p in files), 'conflicting alternatives lost: ' + name)
    # Straight-line full StrCpy is propagated; unknown branch-dependent paths stay partial.
    copied = build('copied', body='''StrCpy $0 "$INSTDIR\\copied"
SetOutPath "$0"
File "hello.txt"
''')
    extract(copied, {'app/copied/hello.txt': sources['hello.txt']})
    dynamic = build('dynamic', body='''ReadRegStr $0 HKCU "Software\\ExtractFixture" "path"
SetOutPath "$0"
File "hello.txt"
''')
    report, files = extract(dynamic, None, 299)
    check(len(files) == 1 and next(iter(files)).startswith('_unresolved/'), 'dynamic retention')
    branches = build('branches', body='''IfFileExists "$TEMP\\fixture.flag" branch_a branch_b
branch_a:
SetOutPath "$INSTDIR\\a"
File "hello.txt"
Goto done
branch_b:
SetOutPath "$INSTDIR\\b"
File "other.dat"
done:
''')
    extract(branches, {'app/a/hello.txt': sources['hello.txt'], 'app/b/other.dat': sources['other.dat']})
    join = build('join', body='''SetOutPath "$INSTDIR\\a"
IfFileExists "$TEMP\\fixture.flag" done 0
SetOutPath "$INSTDIR\\b"
done:
File "hello.txt"
''')
    extract(join, None, 299)
    same_join = build('same-directory-join', body='''SetOutPath "$INSTDIR"
IfFileExists "$TEMP\\flag" branch_a branch_b
branch_a:
DetailPrint "a"
Goto done
branch_b:
DetailPrint "b"
done:
File "hello.txt"
''')
    extract(same_join, {'app/hello.txt': sources['hello.txt']})
    call_keep = build('call-preserves-directory', body='SetOutPath "$INSTDIR"\nCall KeepDirectory\nFile "hello.txt"',
                      extra='Function KeepDirectory\nStrCpy $0 "x"\nPush $0\nPop $0\nFunctionEnd')
    extract(call_keep, {'app/hello.txt': sources['hello.txt']})
    call_changes = build('call-changes-directory', body='SetOutPath "$INSTDIR"\nCall ChangeDirectory\nFile "hello.txt"',
                         extra='Function ChangeDirectory\nSetOutPath "$INSTDIR\\other"\nFunctionEnd')
    extract(call_changes, None, 299)
    out_assignment = build('outdir-dynamic-write', body='SetOutPath "$INSTDIR"\nReadRegStr $OUTDIR HKCU "Software\\Fixture" "Path"\nFile "hello.txt"')
    extract(out_assignment, None, 299)
    # Skip uninstaller construction, retain only actual File payloads.
    uninstall = build('uninstaller', body='SetOutPath "$INSTDIR"\nFile "hello.txt"\nWriteUninstaller "$INSTDIR\\uninstall.exe"',
                      extra='Section "Uninstall"\nDelete "$INSTDIR\\hello.txt"\nSectionEnd')
    extract(uninstall, {'app/hello.txt': sources['hello.txt']})
    # A signed PE may append a certificate after the NSIS-declared archive end.
    signed = root / 'appended-certificate.exe'
    signed.write_bytes(base.read_bytes() + b'FAKE-CERTIFICATE-TEST-ONLY')
    extract(signed, expected)
    invalid = 0

    def reject(path, code=13):
        nonlocal invalid
        before = set(root.iterdir())
        run([path], code)
        check(set(root.iterdir()) == before, f'failed extraction left output: {path}')
        invalid += 1

    def mutate(name, change, code=13, raw=False):
        package = Package(base)
        if raw:
            data = bytearray(package.raw)
            change(data, package)
        else:
            change(package)
            data = package.rebuild()
        path = root / ('bad-' + name + '.exe')
        path.write_bytes(data)
        reject(path, code)

    mutate('crc', lambda d, p: d.__setitem__(-1, d[-1] ^ 1), raw=True)
    mutate('truncation', lambda d, p: d.__delitem__(slice(-100, None)), raw=True)
    mutate('header-budget', lambda d, p: struct.pack_into('<I', d, p.start + 20, 65 * 1024**2), 223, True)
    mutate('opcode', lambda p: struct.pack_into('<I', p.header, p.file_entries[0], 999), 50)
    mutate('instruction-count', lambda p: struct.pack_into('<I', p.header, 24, 100001), 223)
    mutate('file-boundary', lambda p: struct.pack_into('<I', p.header, p.file_entries[0] + 12, 1))
    mutate('negative-offset', lambda p: struct.pack_into('<I', p.header, p.file_entries[0] + 12, 0xffffffff))
    mutate('string-index', lambda p: struct.pack_into('<I', p.header, p.file_entries[0] + 8, 0x7fffffff))
    mutate('bad-language', lambda p: struct.pack_into('<i', p.header, p.file_entries[0] + 8, -10000))
    mutate('traversal', lambda p: p.rename('payload.bin', '..\\evil.bin'), 5)
    mutate('ads', lambda p: p.rename('payload.bin', 'pay:oad.bin'), 5)
    mutate('device', lambda p: p.rename('payload.bin', 'CON    .bin'), 5)
    mutate('absolute', lambda p: p.rename('payload.bin', 'C:\\evil.bin'), 5)
    mutate('data-size', lambda p: struct.pack_into('<I', p.data, 0, 0x7fffffff))
    mutate('layout', lambda p: struct.pack_into('<I', p.header, 4, 299), 50)
    # Recompute the package CRC so each decoder, rather than just CRC, sees truncation.
    for fixture in built:
        data = bytearray(fixture.read_bytes())
        start = data.index(b'\xef\xbe\xad\xdeNullsoftInst') - 4
        del data[-12:-4]
        struct.pack_into('<I', data, start + 24, len(data) - start)
        struct.pack_into('<I', data, len(data) - 4, zlib.crc32(data[512:-4]))
        target = root / ('bad-stream-' + fixture.name)
        target.write_bytes(data)
        reject(target)
    ansi = build('ansi', unicode=False, body='SetOutPath "$INSTDIR"\nFile "hello.txt"')
    reject(ansi, 50)
    # Independently translate a stored official 3.x ANSI fixture to the documented
    # 2.x FC/FD/FE/FF encoding. This is a derived fixture, not a 2.x compiler claim.
    ansi_stored = build('ansi-stored', unicode=False, stored=True,
        body='SetOutPath "$(FixtureDir)"\nFile /oname=abc.dat "hello.txt"',
        extra='LoadLanguageFile "${NSISDIR}\\Contrib\\Language files\\English.nlf"\n'
              'LangString FixtureDir ${LANG_ENGLISH} "$INSTDIR\\sub"')
    def legacy_ansi():
        p = Package(ansi_stored)
        end = struct.unpack_from('<I', p.header, 36)[0]
        i = p.strings
        while i < end:
            code = p.header[i]
            if 1 <= code <= 4:
                p.header[i] = 256 - code
                i += 2 if code == 4 else 3
            else:
                check(code < 252, 'fixture requires ANSI byte escaping')
                i += 1
        return p
    def ansi_case(name, change, expected_files=None, error=None):
        p = legacy_ansi()
        change(p)
        target = root / ('ansi2-' + name + '.exe')
        target.write_bytes(p.rebuild())
        if error is not None:
            reject(target, error)
        else:
            report, _ = extract(target, expected_files)
            check('2.' in report['formatVersion'], 'ANSI legacy layout report')
    def ansi_name(p, value, language=2052):
        check(len(value) == 7, 'ANSI replacement width')
        at = p.header.index(b'abc.dat', p.strings)
        p.header[at:at + 7] = value
        lang = struct.unpack_from('<I', p.header, 36)[0]
        struct.pack_into('<H', p.header, lang, language)
    ansi_case('ascii', lambda p: None, {'app/sub/abc.dat': sources['hello.txt']})
    # CP936 character whose second byte is FD: unescape before multibyte decoding.
    chinese_name = b'\x81\xfd'.decode('cp936') + '.dat'
    ansi_case('chinese-escaped', lambda p: ansi_name(p, b'\x81\xfc\xfd.dat'),
              {'app/sub/' + chinese_name: sources['hello.txt']})
    ansi_case('western', lambda p: ansi_name(p, b'\xe9bc.dat', 1033),
              {'app/sub/ébc.dat': sources['hello.txt']})
    ansi_case('unknown-page-ascii', lambda p: ansi_name(p, b'abc.dat', 65535),
              {'app/sub/abc.dat': sources['hello.txt']})
    ansi_case('unknown-page-text', lambda p: ansi_name(p, b'\x81\xfc\xfd.dat', 65535), error=50)
    ansi_case('invalid-multibyte', lambda p: ansi_name(p, b'\x81\x30\x81.dat'), error=13)
    ansi_case('traversal', lambda p: ansi_name(p, b'../.dat'), error=5)
    def truncated_ansi(p):
        end = struct.unpack_from('<I', p.header, 36)[0]
        p.header[end - 2] = 255
    ansi_case('truncated-code', truncated_ansi, error=13)
    def long_ansi(p):
        end = struct.unpack_from('<I', p.header, 36)[0]
        text = b'a' * 32001 + b'\0'
        p.header[end:end] = text
        for block in range(4, 8):
            at = 4 + block * 8
            offset = struct.unpack_from('<I', p.header, at)[0]
            if offset >= end:
                struct.pack_into('<I', p.header, at, offset + len(text))
        struct.pack_into('<I', p.header, p.file_entries[0] + 8, end - p.strings)
        struct.pack_into('<I', p.raw, p.start + 20, len(p.header))
        struct.pack_into('<I', p.raw, p.header_start - 4, len(p.header))
    ansi_case('literal-budget', long_ansi, error=223)
    # Malformed control flow must be rejected before payload creation.
    branch_package = Package(base)
    struct.pack_into('<7i', branch_package.header, branch_package.entries, 2, branch_package.count + 100, 0, 0, 0, 0, 0)
    bad_branch = root / 'bad-branch.exe'
    bad_branch.write_bytes(branch_package.rebuild())
    reject(bad_branch)
    # Supply a valid outer frame and a bzip2 header with an oversized selector count.
    single = build('single-stored', stored=True, body='SetOutPath "$INSTDIR"\nFile "hello.txt"')
    for groups, selectors in ((2, 32767), (0, 1)):
        bits = '1000000000000000' * 2 + f'{groups:03b}' + f'{selectors:015b}'
        bits += '0' * (-len(bits) % 8)
        bad_bzip = b'\x31\0\0\0' + int(bits, 2).to_bytes(len(bits) // 8, 'big')
        package = Package(single)
        package.data = struct.pack('<I', 0x80000000 | len(bad_bzip)) + bad_bzip
        path = root / f'bad-bzip-tables-{groups}-{selectors}.exe'
        path.write_bytes(package.rebuild())
        reject(path)
    # A flipped compressed bit can still describe valid data after a recomputed CRC.
    # This corpus checks controlled outcomes; it does not assume all mutations are invalid.
    mutation_results = []
    for fixture in (p for p in built if p.stem.endswith('-solid')):
        original = fixture.read_bytes()
        start = original.index(b'\xef\xbe\xad\xdeNullsoftInst') - 4
        for delta in (0, 1, 3, 7, 15, 31, 63, 127):
            data = bytearray(original)
            data[start + 28 + delta] ^= 0xff
            struct.pack_into('<I', data, len(data) - 4, zlib.crc32(data[512:-4]))
            path = root / f'bad-bit-{fixture.stem}-{delta}.exe'
            path.write_bytes(data)
            result = subprocess.run([str(executable), '--quiet', '--list', str(path)], capture_output=True, timeout=30, env=environment)
            run_count += 1
            check(not list(cache.iterdir()), 'mutation cache leak')
            check(result.returncode in (0, 5, 13, 50, 223), f'mutation crash: {path}: {result.returncode}')
            if result.returncode == 0:
                check(json.loads(result.stdout)['format'] == 'NSIS', 'mutation listing')
            mutation_results.append(result.returncode)
    run([root / 'bad-crc.exe', base], 299)
    evidence = {'compiler': str(compiler), 'version': version, 'matrixPackages': len(built),
                'invalidCases': invalid, 'mutationCases': len(mutation_results), 'mutationResults': mutation_results,
                'extractInvocations': run_count, 'fixtures': str(root)}
    (root / 'validation.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(evidence, ensure_ascii=False), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
