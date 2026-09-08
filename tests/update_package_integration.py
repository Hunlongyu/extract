"""Static update-package fixtures. Execute only Extract; never launch any input/output EXE."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import uuid
import zipfile

MAGIC = hashlib.sha256(b'squirrel bundle').digest()


def zipped(files, method=zipfile.ZIP_DEFLATED):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', compression=method) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    return stream.getvalue()


def pe(section, payload, resource=False):
    """Inert PE with one section and no entry point; no executable code."""
    raw_size = (len(payload) + 511) & ~511
    data = bytearray(512 + raw_size)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, 0x8664, 1)
    struct.pack_into('<H', data, 148, 240)
    opt = 152
    struct.pack_into('<H', data, opt, 0x20b)
    struct.pack_into('<II', data, opt + 32, 4096, 512)
    struct.pack_into('<II', data, opt + 56, 4096 + ((raw_size + 4095) & ~4095), 512)
    struct.pack_into('<I', data, opt + 108, 16)
    if resource:
        struct.pack_into('<II', data, opt + 112 + 16, 4096, len(payload))
    data[392:392 + len(section)] = section
    struct.pack_into('<IIII', data, 400, len(payload), 4096, raw_size, 512)
    data[512:512 + len(payload)] = payload
    return data


def velo(package, signed=False):
    data = pe(b'.rdata', bytes(16) + MAGIC)
    struct.pack_into('<QQ', data, 512, len(data), len(package))
    data.extend(package)
    if signed:
        data.extend(bytes((-len(data)) % 8))
        struct.pack_into('<II', data, 152 + 112 + 32, len(data), 16)
        data.extend(struct.pack('<IHH', 16, 0x200, 2) + bytes(8))
    return data


def squirrel(outer):
    resource = bytearray(112)
    struct.pack_into('<HHII', resource, 12, 1, 0, 0x80000000 | 88, 0x80000000 | 24)
    struct.pack_into('<HHII', resource, 36, 0, 1, 131, 0x80000000 | 48)
    struct.pack_into('<HHII', resource, 60, 0, 1, 0x409, 72)
    struct.pack_into('<II', resource, 72, 4096 + 112, len(outer))
    resource[88:98] = struct.pack('<H', 4) + 'DATA'.encode('utf-16-le')
    return pe(b'.rsrc', resource + outer, resource=True)


def nuspec(velopack=True, extra=''):
    return ('<?xml version="1.0" encoding="utf-8"?>'
            '<package xmlns="http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd">'
            '<metadata><id>StaticFixture</id><version>1.2.3</version>'
            + ('<os>win</os><mainExe>Application.exe</mainExe>' if velopack else '')
            + extra + '</metadata></package>').encode()


def outer_zip(package, **overrides):
    name = 'StaticFixture-1.2.3-full.nupkg'
    releases = f'{hashlib.sha1(package).hexdigest()} {name} {len(package)}\r\n'.encode()
    return zipped({'Update.exe': b'inert updater', 'RELEASES': releases, name: package, **overrides})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    parser.add_argument('--reference-root', type=Path)
    args = parser.parse_args()
    exe = args.executable.resolve()
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    cache = root / 'cache'; cache.mkdir()
    environment = dict(os.environ, TEMP=str(cache), TMP=str(cache))
    results = []

    def check(value, message):
        if not value:
            raise AssertionError(message)

    def run(name, data, expected=None, code=0, fmt=None, message=None):
        path = root / name
        path.write_bytes(data)
        output = root / ('output-' + uuid.uuid4().hex); output.mkdir()
        process = subprocess.run([exe, '--quiet', '--output', output, path],
                                 capture_output=True, timeout=120, env=environment)
        check(process.returncode == code, f'{name}: expected {code}, got {process.returncode}: {process.stderr!r}')
        check(path.read_bytes() == data, f'{name}: input changed')
        check(not list(cache.iterdir()), f'{name}: leaked cache')
        if message:
            check(message in process.stderr.decode('utf-8'), f'{name}: missing error detail')
        results.append({'name': name, 'exitCode': code, 'sha256': hashlib.sha256(data).hexdigest()})
        if code:
            check(not list(output.iterdir()), f'{name}: failure left output')
            return
        targets = list(output.iterdir())
        check(len(targets) == 1, 'one output directory')
        target = targets[0]
        report = json.loads((target / '_extract-report.json').read_text(encoding='utf-8'))
        if fmt:
            check(report['format'] == fmt, f'{name}: incorrect format {report["format"]}')
        actual = {}
        for entry in report['files']:
            body = (target / entry['path']).read_bytes()
            check(hashlib.sha256(body).hexdigest() == entry['sha256'], 'report hash')
            check(entry['sourceHashVerified'], 'missing source CRC verification')
            actual[entry['path']] = body
        if expected is not None:
            check(actual == expected, f'{name}: layout/content mismatch: {list(actual)}')
        if fmt and fmt != 'ZIP':
            check(str(target / 'app') in process.stdout.decode('utf-8'), f'{name}: primary folder is not app')
        results[-1].update(format=report['format'], files=len(actual))
        return target, report

    app = {'Application.exe': b'INERT application bytes, never executed',
           'resources/app.asar': b'opaque application archive',
           '中文/说明.txt': 'Unicode 名称 😀'.encode(), 'space file.txt': b'spaces',
           'percent%20literal.txt': b'not URI encoded', 'empty.dat': b''}
    raw = {'[Content_Types].xml': b'<Types/>', 'StaticFixture.nuspec': nuspec(),
           **{'lib/app/' + k: v for k, v in app.items()},
           'lib/app/Squirrel.exe': b'inert updater', 'lib/app/Application_ExecutionStub.exe': b'inert stub'}
    wanted = {'app/' + k: v for k, v in app.items()}
    wanted.update({'app/sq.version': nuspec(), '_package/[Content_Types].xml': b'<Types/>',
                   '_package/lib/app/Squirrel.exe': b'inert updater',
                   '_package/lib/app/Application_ExecutionStub.exe': b'inert stub'})
    package = zipped(raw)
    run('Velopack 离线包.exe', velo(package), wanted, fmt='Velopack')
    run('signed-layout.exe', velo(package, signed=True), wanted, fmt='Velopack')
    run('StaticFixture-1.2.3-full.nupkg', package, wanted, fmt='Velopack')
    run('stored.exe', velo(zipped(raw, zipfile.ZIP_STORED)), wanted, fmt='Velopack')
    # Already-supplied sq.version wins; root nuspec remains packaging metadata.
    run('with-sq-version.exe', velo(zipped({**raw, 'lib/app/sq.version': b'original version'})),
        {**wanted, 'app/sq.version': b'original version', '_package/StaticFixture.nuspec': nuspec()}, fmt='Velopack')
    squirrel_raw = {**raw, 'StaticFixture.nuspec': nuspec(False)}
    squirrel_wanted = {**wanted, '_package/StaticFixture.nuspec': nuspec(False), 'app/Squirrel.exe': b'inert updater'}
    del squirrel_wanted['app/sq.version'], squirrel_wanted['_package/lib/app/Squirrel.exe']
    run('Squirrel 离线包.exe', squirrel(outer_zip(zipped(squirrel_raw))), squirrel_wanted, fmt='Squirrel.Windows')
    run('Squirrel-1.2.3-full.nupkg', zipped(squirrel_raw), squirrel_wanted, fmt='Squirrel release package')
    multiple = {'StaticFixture.nuspec': nuspec(False), 'lib/net45/Application.exe': b'x86', 'lib/net48/Application.exe': b'x64'}
    run('Multiple-1.2.3-full.nupkg', zipped(multiple),
        {'_package/StaticFixture.nuspec': nuspec(False), 'app/net45/Application.exe': b'x86', 'app/net48/Application.exe': b'x64'}, fmt='Squirrel release package')
    # Ordinary NuGet and ZIP names are not silently reinterpreted as installers.
    run('Library.1.0.nupkg', zipped(squirrel_raw), squirrel_raw, fmt='ZIP')
    run('ordinary.zip', zipped({'Extract.exe': exe.read_bytes(), 'notes.txt': b'ordinary'}),
        {'Extract.exe': exe.read_bytes(), 'notes.txt': b'ordinary'}, fmt='ZIP')
    nested = run('wrapper.zip', zipped({'inner.exe': bytes(velo(package))}), fmt='ZIP')
    check(nested[1]['nestedPackages'][0]['status'] == 'complete', 'nested installer not expanded')
    check((nested[0] / nested[1]['nestedPackages'][0]['output'] / 'app/resources/app.asar').read_bytes() == app['resources/app.asar'], 'inner final application')
    for patch in ('.diff', '.bsdiff', '.zsdiff'):
        changed = {**raw, 'lib/app/Application.exe' + patch: b'patch data'}
        run('renamed-delta-' + patch[1:] + '.exe', velo(zipped(changed)), code=50, message='差分')
    run('StaticFixture-1.2.3-delta.nupkg', package, code=50, message='差分')
    run('delta-only.exe', squirrel(zipped({'Update.exe': b'x', 'App-delta.nupkg': b'patch'})), code=50, message='差分')
    run('online.exe', squirrel(zipped({'Update.exe': b'bootstrap', 'RELEASES': b''})), code=50, message='完整离线载荷')
    empty = pe(b'.rdata', bytes(16) + MAGIC)
    run('empty-velopack.exe', empty, code=50, message='没有内嵌完整载荷')
    run('two-full.exe', squirrel(zipped({'A-full.nupkg': package, 'B-full.nupkg': package})), code=50, message='多个完整包')
    run('missing-releases.exe', squirrel(zipped({'Update.exe': b'x', 'A-full.nupkg': package})), code=13, message='RELEASES')
    bad_release = f'{"0" * 40} StaticFixture-1.2.3-full.nupkg {len(package)}'.encode()
    run('bad-release-hash.exe', squirrel(outer_zip(package, RELEASES=bad_release)), code=13, message='SHA-1')
    bad_release = f'{hashlib.sha1(package).hexdigest()} StaticFixture-1.2.3-full.nupkg 1'.encode()
    run('bad-release-size.exe', squirrel(outer_zip(package, RELEASES=bad_release)), code=13, message='大小')
    for bad_name in ('lib/app/../escape.txt', 'lib/app/file:stream', 'lib/app/CON.txt'):
        run('path-' + uuid.uuid4().hex + '.exe', velo(zipped({**raw, bad_name: b'bad'})), code=5)
    run('symlink.exe', velo(zipped({**raw, 'lib/app/link.__symlink': b'Application.exe'})), code=50, message='符号链接')
    for label, spec in [('dtd', b'<!DOCTYPE package [<!ENTITY x SYSTEM "file:///C:/secret">]>' + nuspec().split(b'?>', 1)[1]),
                        ('duplicate', nuspec(extra='<version>9</version>')),
                        ('nested-field', nuspec().replace(b'<id>StaticFixture</id>', b'<id>good<bad/></id>')),
                        ('missing-main', nuspec().replace(b'Application.exe', b'Absent.exe'))]:
        run(label + '.exe', velo(zipped({**raw, 'StaticFixture.nuspec': spec})), code=13)
    run('linux.exe', velo(zipped({**raw, 'StaticFixture.nuspec': nuspec().replace(b'<os>win', b'<os>linux')})), code=50)
    run('large-metadata.exe', velo(zipped({**raw, 'StaticFixture.nuspec': bytes(4 * 1024**2 + 1)})), code=223)
    damaged = velo(package); struct.pack_into('<Q', damaged, 512, 0xffffffffffffffff)
    run('offset-overflow.exe', damaged, code=13)
    damaged = velo(package); struct.pack_into('<Q', damaged, 520, len(package) + 100)
    run('length-overflow.exe', damaged, code=13)
    damaged = velo(package, signed=True); struct.pack_into('<I', damaged, 152 + 112 + 32, len(damaged) - 40)
    run('certificate-overlap.exe', damaged, code=13)
    damaged = velo(package); damaged[1024 + 40] ^= 1
    run('crc-or-header.exe', damaged, code=13)
    damaged = pe(b'.rdata', bytes(16) + MAGIC + bytes(16) + MAGIC)
    run('duplicate-marker.exe', damaged, code=13)
    collision_raw = {**raw, 'lib/app/NAME.txt': b'first', 'lib/app/name.txt': b'second'}
    _, collision = run('case-collision.exe', velo(zipped(collision_raw)), fmt='Velopack')
    variants = [f for f in collision['files'] if f['originalPath'].lower().endswith('/name.txt')]
    check(len(variants) == 2 and all(f['path'].startswith('_variants/') for f in variants), 'case collision overwritten')
    damaged = squirrel(outer_zip(package)); struct.pack_into('<I', damaged, 512 + 16, 0xffffffff)
    run('resource-name-overflow.exe', damaged, code=13)
    damaged = squirrel(outer_zip(package)); struct.pack_into('<I', damaged, 512 + 72, 0xfffffff0)
    run('resource-data-overflow.exe', damaged, code=13)
    # Official project fixtures are optional local validation, not downloaded by CTest.
    if args.reference_root:
        refs = args.reference_root.resolve()
        # Current official 1.2.0 setup template: append an inert known package and
        # patch its documented header, without executing the packager or stub.
        stub = bytearray((refs / 'vpk-1.2.0-vendor-Setup.exe').read_bytes())
        check(hashlib.sha256(stub).hexdigest() == '715d20a329bf1315904219d5a20fc92f8a555a60308d2d478b258c4acc683b80', 'current official stub hash')
        pos = stub.index(MAGIC)
        check(stub[pos - 16:pos] == bytes(16) and stub.count(MAGIC) == 1, 'official placeholder')
        struct.pack_into('<QQ', stub, pos - 16, len(stub), len(package)); stub.extend(package)
        run('official-velopack-1.2.0-inert.exe', stub, wanted, fmt='Velopack')
        for name, sha in {
            'LegacyTestApp-Velopack1298-Setup.exe': 'fca012feb5482751b24758db9a79c00fc48b4b3d989fec4cd6a4bc80545be626',
            'LegacyTestApp-SquirrelWinV2-Setup.exe': '349509cc90e0797f2665eaed4571593767359a3fa19767d1b755565d17d7e626',
            'AvaloniaCrossPlat-1.0.15-win-full.nupkg': '55572e5b8c3c6d22a621e8b643d5669e10364d0fa266c4f7b7bd9cf520192d0b',
        }.items():
            body = (refs / name).read_bytes()
            check(hashlib.sha256(body).hexdigest() == sha, 'reference fixture hash')
            target, report = run(name, body)
            # Python ZIP implementation independently validates every original byte.
            if name.endswith('.nupkg'):
                archive_bytes = body
            elif 'Velopack' in name:
                pos = body.index(MAGIC); offset, size = struct.unpack_from('<QQ', body, pos - 16)
                archive_bytes = body[offset:offset + size]
            else:
                # The known official DATA/131 ZIP is the unique outer ZIP ending in this PE.
                # zipfile supports PE-prefix bias without using Extract's resource parser.
                outer = zipfile.ZipFile(io.BytesIO(body))
                archive_bytes = outer.read(next(n for n in outer.namelist() if n.endswith('-full.nupkg')))
            archive = zipfile.ZipFile(io.BytesIO(archive_bytes))
            check(len(report['files']) == len([n for n in archive.namelist() if not n.endswith('/')]), 'official file count')
            for entry in report['files']:
                check((target / entry['path']).read_bytes() == archive.read(entry['sourceExpression']), 'official member differs')
        name = 'Clowd-3.4.288-delta.nupkg'
        delta = (refs / name).read_bytes()
        check(hashlib.sha256(delta).hexdigest() == '6a5bd0f6a92d8a79a7fc5c6d2eb10709bd3803e5c237a2064be1908ad597688b', 'official delta hash')
        run(name, delta, code=50, message='差分')
    (root / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'PASS: {len(results)} update-package cases; {root}', flush=True)


if __name__ == '__main__':
    main()
