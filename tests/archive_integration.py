"""ZIP/7z fixtures and hostile metadata; execute only official packer and Extract."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import zipfile
import zlib


def check(value, message):
    if not value:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--work-root', required=True, type=Path)
    parser.add_argument('--sevenzip', required=True, type=Path)
    parser.add_argument('--nsis-compiler', type=Path)
    args = parser.parse_args()
    if not args.sevenzip.is_file():
        print('SKIP: verified official 7zr.exe required')
        return 77
    exe, packer = args.executable.resolve(), args.sevenzip.resolve()
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    print(f'Fixtures: {root}', flush=True)
    source = root / 'source'
    source.mkdir()
    expected = {'hello.txt': b'fixture\n' * 100, 'bin/payload.dat': bytes(range(256)) * 8192,
                '中文/说明.txt': '中文数据 😀\n'.encode(), 'empty.dat': b''}
    for name, data in expected.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    cache = root / 'cache'
    cache.mkdir()
    environment = dict(os.environ, TEMP=str(cache), TMP=str(cache))
    results = []
    stub = bytearray(exe.read_bytes())
    pe = struct.unpack_from('<I', stub, 60)[0]
    count, optional = struct.unpack_from('<H', stub, pe + 6)[0], struct.unpack_from('<H', stub, pe + 20)[0]
    end = max(sum(struct.unpack_from('<II', stub, pe + 24 + optional + i * 40 + 16)) for i in range(count))
    stub = stub[:end]  # Test-only PE stub; no SFX program or output is executed.

    def run(path, wanted=expected, code=0):
        output = root / ('result-' + uuid.uuid4().hex)
        output.mkdir()
        before = hashlib.sha256(path.read_bytes()).digest()
        process = subprocess.run([str(exe), '--quiet', '--output', str(output), str(path)], capture_output=True, timeout=120, env=environment)
        check(process.returncode == code, f'{path.name}: expected {code}, got {process.returncode}: {process.stderr!r}')
        check(not list(cache.iterdir()), 'archive temporary cache residue')
        check(hashlib.sha256(path.read_bytes()).digest() == before, 'input modified')
        results.append({'name': path.name, 'exitCode': code})
        if code not in (0, 299):
            check(not list(output.iterdir()), 'failed extraction left result/staging')
            return None
        dirs = list(output.iterdir())
        check(len(dirs) == 1 and '.tmp' not in dirs[0].name, 'output commit')
        target = dirs[0]
        results[-1]['primaryOutput'] = process.stdout.decode('utf-8').splitlines()[0].partition('：')[2]
        report = json.loads((target / '_extract-report.json').read_text(encoding='utf-8'))
        actual = {}
        for item in report['files']:
            data = (target / item['path']).read_bytes()
            check(len(data) == item['size'], 'file size')
            check(hashlib.sha256(data).hexdigest() == item['sha256'], 'file hash')
            if report['format'] in ('7z', 'ZIP') and data:
                check(item['sourceHashAlgorithm'] == 'CRC-32' and item['sourceHashVerified'], 'file source CRC')
            actual[item['path']] = data
        if wanted is not None:
            check(actual == wanted, f'known content mismatch {path.name}')
        return target, report

    def zip_bytes(method=zipfile.ZIP_DEFLATED, files=expected, stream=False, force64=False):
        class Sink(io.BytesIO):
            def seekable(self): return False
            def seek(self, *args): raise io.UnsupportedOperation('stream')
        buffer = Sink() if stream else io.BytesIO()
        with zipfile.ZipFile(buffer, 'w', compression=method) as archive:
            for name, data in files.items():
                if force64:
                    with archive.open(name, 'w', force_zip64=True) as file: file.write(data)
                else: archive.writestr(name, data)
        return buffer.getvalue()

    def write(name, data):
        path = root / name
        path.write_bytes(data)
        return path

    # A tiny valid 7z header declaring a >4 GiB Copy block. List only: never
    # allocate/decode its deliberately absent payload. x86 must reject before
    # narrowing the block size to size_t; 64-bit builds can represent this size.
    large_name = b'\x00' + 'large.bin\x00'.encode('utf-16-le')
    large_header = (bytes.fromhex('0104060001090100070b01000101000c') + b'\xff'
                    + struct.pack('<Q', 9 * 1024**3) + bytes.fromhex('0000050111')
                    + bytes([len(large_name)]) + large_name + b'\x00\x00')
    large_start = struct.pack('<QQI', 1, len(large_header), zlib.crc32(large_header))
    large_archive = (b'7z\xbc\xaf\x27\x1c\x00\x04' + struct.pack('<I', zlib.crc32(large_start))
                     + large_start + b'\x00' + large_header)
    large_path = write('large-block-metadata.7z', large_archive)
    listed = subprocess.run([str(exe), '--list', str(large_path)], capture_output=True,
                            timeout=30, env=environment)
    machine = struct.unpack_from('<H', stub, pe + 4)[0]
    expected_code = 223 if machine == 0x14c else 0
    check(listed.returncode == expected_code, '7z block size narrowed on current architecture')
    results.append({'name': large_path.name, 'exitCode': expected_code, 'mode': 'list'})

    for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED, zipfile.ZIP_BZIP2):
        run(write(f'method-{method}.zip', zip_bytes(method)))
    run(write('descriptor.zip', zip_bytes(stream=True)))
    run(write('zip64-local.zip', zip_bytes(force64=True)))
    run(write('zip64-descriptor.zip', zip_bytes(stream=True, force64=True)))
    ordinary = zip_bytes(files={'normal.exe': exe.read_bytes()})
    _, report = run(write('ordinary-app.zip', ordinary), {'normal.exe': exe.read_bytes()})
    check(not report['nestedPackages'], 'ordinary application mistaken for SFX')
    run(write('zip-sfx.exe', bytes(stub) + zip_bytes()))
    run(write('empty.zip', zip_bytes(files={})), {})
    run(write('directories.zip', zip_bytes(files={'empty/': b'', **expected})))
    duplicates = zip_bytes(files={'Same.txt': b'A', 'same.txt': b'B'})
    _, report = run(write('variants.zip', duplicates), None)
    check(all(x['path'].startswith('_variants/') for x in report['files']), 'case variants')

    base = bytearray(zip_bytes(files={'hello.txt': b'known payload'}, method=zipfile.ZIP_STORED))
    cd, eocd = base.index(b'PK\x01\x02'), base.index(b'PK\x05\x06')
    def mutation(name, edit, code=13):
        data = bytearray(base)
        edit(data)
        run(write(name + '.zip', data), None, code)
    mutation('payload-crc', lambda b: b.__setitem__(39, b[39] ^ 1))
    mutation('name-mismatch', lambda b: b.__setitem__(30, ord('X')))
    mutation('local-flags', lambda b: struct.pack_into('<H', b, 6, 8))
    mutation('central-offset', lambda b: struct.pack_into('<I', b, cd + 42, 0xfffffff0))
    mutation('size-limit', lambda b: struct.pack_into('<I', b, eocd + 12, 65 * 1024**2), 223)
    mutation('truncated', lambda b: b.__delitem__(slice(-3, None)))
    mutation('encryption', lambda b: struct.pack_into('<H', b, cd + 8, 1), 50)
    mutation('unknown-method', lambda b: struct.pack_into('<H', b, cd + 10, 99), 50)
    mutation('volume', lambda b: struct.pack_into('<H', b, eocd + 4, 1), 50)
    mutation('symlink', lambda b: struct.pack_into('<I', b, cd + 38, 0xa1ff << 16), 5)
    mutation('reparse', lambda b: struct.pack_into('<I', b, cd + 38, 0x400), 5)
    for name in ('../outside.txt', 'C:/outside.txt', 'file:stream', 'CON.txt'):
        run(write('bad-path-' + uuid.uuid4().hex + '.zip', zip_bytes(files={name: b'x'})), None, 5)

    # ZIP64 EOCD + central size/offset extras, with no multi-gigabyte test files.
    data = bytearray(base[:cd]); record = bytearray(base[cd:eocd])
    packed, size = struct.unpack_from('<II', record, 20)
    extra = struct.pack('<HHQQQ', 1, 24, size, packed, 0)
    struct.pack_into('<II', record, 20, 0xffffffff, 0xffffffff)
    struct.pack_into('<H', record, 30, len(extra)); struct.pack_into('<I', record, 42, 0xffffffff)
    record += extra
    data += record; zip64_at = len(data)
    data += struct.pack('<IQHHIIQQQQ', 0x06064b50, 44, 45, 45, 0, 0, 1, 1, len(record), cd)
    data += struct.pack('<IIQI', 0x07064b50, 0, zip64_at, 1)
    data += struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, 0xffff, 0xffff, 0xffffffff, 0xffffffff, 0)
    run(write('zip64.zip', data), {'hello.txt': b'known payload'})
    run(write('zip64-sfx.exe', bytes(stub) + data), {'hello.txt': b'known payload'})

    seven = []
    for method in ('Copy', 'LZMA', 'LZMA2'):
        for solid in ('off', 'on'):
            path = root / f'{method}-{solid}.7z'
            p = subprocess.run([str(packer), 'a', '-t7z', f'-m0={method}', '-ms=' + solid, str(path), '.'],
                               cwd=source, capture_output=True, timeout=90)
            check(p.returncode == 0, f'7zr packing: {p.stdout!r} {p.stderr!r}')
            run(path); seven.append(path)
    run(write('7z-sfx.exe', bytes(stub) + b';!@Install@!UTF-8!\nRunProgram="never-run.exe"\n;!@InstallEnd@!\n' + seven[-1].read_bytes()))
    damaged = bytearray(seven[-1].read_bytes()); damaged[12] ^= 1
    run(write('bad-start-crc.7z', damaged), None, 13)
    damaged = bytearray(seven[-1].read_bytes()); damaged[-1] ^= 1
    run(write('bad-next-crc.7z', damaged), None, 13)
    run(write('truncated.7z', seven[-1].read_bytes()[:31]), None, 13)
    # Stored folder: corrupt the actual byte stream while keeping metadata CRCs intact.
    damaged = bytearray(seven[0].read_bytes()); position = damaged.index(expected['hello.txt']); damaged[position] ^= 1
    run(write('bad-file-crc.7z', damaged), None, 13)
    # Independently construct a small stored 7z to exercise hostile metadata beyond CRC checks.
    def number(value):
        for count in range(8):
            if value < 1 << (7 + 7 * count):
                first = (0xff << (8 - count)) & 0xff if count else 0
                return bytes([first | (value >> (8 * count))]) + value.to_bytes(8, 'little')[:count]
        return b'\xff' + struct.pack('<Q', value)
    def stored_seven(name='file.txt', size=None, attr=None, method=0):
        payload = b'independent fixture'
        length = len(payload) if size is None else size
        streams = b'\x04\x06\0\x01\x09' + number(len(payload)) + b'\0'
        streams += b'\x07\x0b\x01\0\x01\x01' + bytes([method]) + b'\x0c' + number(length)
        streams += b'\x0a\x01' + struct.pack('<I', zlib.crc32(payload)) + b'\0\0'
        encoded_name = b'\0' + name.encode('utf-16le') + b'\0\0'
        files = b'\x05\x01\x11' + number(len(encoded_name)) + encoded_name
        if attr is not None: files += b'\x15\x06\x01\0' + struct.pack('<I', attr)
        header = b'\x01' + streams + files + b'\0\0'
        start = struct.pack('<QQI', len(payload), len(header), zlib.crc32(header))
        return b'7z\xbc\xaf\x27\x1c\0\x04' + struct.pack('<I', zlib.crc32(start)) + start + payload + header
    independent = write('independent.7z', stored_seven())
    checked = subprocess.run([str(packer), 't', str(independent)], capture_output=True, timeout=30)
    check(checked.returncode == 0, f'independent format fixture invalid: {checked.stdout!r}')
    run(independent, {'file.txt': b'independent fixture'})
    run(write('7z-integer-overflow.7z', stored_seven(size=1 << 63)), None, 223)
    run(write('7z-unsafe.7z', stored_seven(name='../outside.txt')), None, 5)
    run(write('7z-symlink.7z', stored_seven(attr=0xa1ff << 16)), None, 5)
    run(write('7z-reparse.7z', stored_seven(attr=0x400)), None, 5)
    run(write('7z-unknown-method.7z', stored_seven(method=0x7f)), None, 50)
    encrypted = root / 'encrypted.7z'
    p = subprocess.run([str(packer), 'a', '-t7z', '-pfixture', '-mhe=on', str(encrypted), '.'], cwd=source, capture_output=True, timeout=90)
    if p.returncode == 0: run(encrypted, None, 50)
    for filters in (['-m0=BCJ', '-m1=LZMA2'], ['-m0=BCJ2', '-m1=LZMA2', '-m2=LZMA2', '-m3=LZMA2', '-mb0:1', '-mb0s1:2', '-mb0s2:3']):
        path = root / ('filter-' + uuid.uuid4().hex + '.7z')
        p = subprocess.run([str(packer), 'a', '-t7z', *filters, str(path), '.'], cwd=source, capture_output=True, timeout=90)
        check(p.returncode == 0, f'filter fixture: {p.stdout!r} {p.stderr!r}')
        run(path)

    if args.nsis_compiler and args.nsis_compiler.is_file():
        for archive in (write('embedded.zip', zip_bytes()), seven[-1]):
            script = root / (archive.name + '.nsi')
            target = root / (archive.name + '.exe')
            script.write_text(f'Unicode true\nName "Embedded archive"\nBrandingText "Custom wrapper"\nOutFile "{target}"\nSection\nSetOutPath "$PLUGINSDIR"\nFile "{archive}"\nSectionEnd\n', encoding='utf-8-sig')
            p = subprocess.run([str(args.nsis_compiler.resolve()), '/V2', str(script)], capture_output=True, timeout=60)
            check(p.returncode == 0, f'NSIS archive fixture: {p.stdout!r} {p.stderr!r}')
            target, report = run(target, None)
            child = target / report['nestedPackages'][0]['output']
            for name, data in expected.items(): check((child / name).read_bytes() == data, 'recursive archive payload')
            check(report['nestedPackages'][0]['status'] == 'complete', 'recursive archive status')
        # A wrapper's app directory may hold only resources such as an uninstaller icon.
        icon = write('uninstallerIcon.ico', b'fixture icon bytes')
        for archive in (write('application.zip', zip_bytes()), seven[-1]):
            for outer_executable in (False, True):
                name = f'primary-{archive.suffix[1:]}-{outer_executable}'
                script = root / (name + '.nsi')
                installer = root / (name + '.exe')
                app_entry = f'File /oname=application.exe "{exe}"\n' if outer_executable else ''
                script.write_text(f'''Unicode true
Name "Application output fixture"
OutFile "{installer}"
Section
SetOutPath "$PLUGINSDIR"
File "{archive}"
SetOutPath "$INSTDIR"
File "{icon}"
{app_entry}SectionEnd
''', encoding='utf-8-sig')
                built = subprocess.run([str(args.nsis_compiler.resolve()), '/V2', str(script)], capture_output=True, timeout=60)
                check(built.returncode == 0, f'primary output fixture: {built.stdout!r} {built.stderr!r}')
                target, report = run(installer, None)
                child = target / report['nestedPackages'][0]['output']
                expected_primary = target / 'app' if outer_executable else child
                check(Path(results[-1]['primaryOutput']) == expected_primary, 'primary output must contain the actual application, not only outer resources')
                check((target / 'app/uninstallerIcon.ico').read_bytes() == icon.read_bytes(), 'outer resources must still be retained')
    (root / 'validation.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(f'PASS: {len(results)} archive scenarios', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
