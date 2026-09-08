"""MSIX/APPX/Bundle static tests; run only SDK MakeAppx pack/bundle and Extract."""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import urllib.parse
import uuid
import xml.etree.ElementTree as ET
import zipfile
import zlib

BLOCK_NS = 'http://schemas.microsoft.com/appx/2010/blockmap'
BUNDLE_NS = 'http://schemas.microsoft.com/appx/2013/bundle'
APP_NS = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'
TYPES = b'<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>'
HASH_URIS = {'sha256': 'http://www.w3.org/2001/04/xmlenc#sha256',
             'sha384': 'http://www.w3.org/2001/04/xmldsig-more#sha384',
             'sha512': 'http://www.w3.org/2001/04/xmlenc#sha512'}


def manifest(arch='x64', resource=False, extra=''):
    return (f'<Package xmlns="{APP_NS}" xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10" IgnorableNamespaces="uap">'
            '<Identity Name="Extract.StaticFixture" Publisher="CN=Extract" Version="1.2.3.4"'
            + (' ResourceId="lang-zh"' if resource else f' ProcessorArchitecture="{arch}"') + '/>'
            '<Properties><DisplayName>Static Fixture</DisplayName><PublisherDisplayName>Extract</PublisherDisplayName><Logo>Assets/logo.png</Logo>'
            + ('<ResourcePackage>true</ResourcePackage>' if resource else '') + '</Properties>'
            '<Resources><Resource Language="' + ('zh-CN' if resource else 'en-US') + '"/></Resources>'
            '<Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0"/></Dependencies>'
            + ('' if resource else '<Applications><Application Id="App" Executable="Application.exe" EntryPoint="StaticFixture.App">'
               '<uap:VisualElements DisplayName="Static Fixture" Description="Inert test payload" BackgroundColor="transparent" Square150x150Logo="Assets/logo.png" Square44x44Logo="Assets/logo.png"/>'
               '</Application></Applications>') + extra + '</Package>').encode()


def zipped(files):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_STORED) as archive:
        for name, value in files.items(): archive.writestr(name, value)
    return stream.getvalue()


def encode_path(name):
    return urllib.parse.quote(name, safe='/.-_~')


def synthetic(files, mutate=None, algorithm='sha256'):
    blockmap = ET.Element('BlockMap', xmlns=BLOCK_NS, HashMethod=HASH_URIS[algorithm])
    for name, body in files.items():
        file = ET.SubElement(blockmap, 'File', Name=name.replace('/', '\\'), Size=str(len(body)),
                             LfhSize=str(30 + len(encode_path(name).encode())))
        for offset in range(0, len(body), 65536):
            value = hashlib.new(algorithm, body[offset:offset + 65536]).digest()
            ET.SubElement(file, 'Block', Hash=base64.b64encode(value).decode())
    if mutate: mutate(blockmap)
    return zipped({**{encode_path(n): b for n, b in files.items()}, 'AppxBlockMap.xml': ET.tostring(blockmap), '[Content_Types].xml': TYPES})


def synthetic_bundle(packages, missing=False, change=None):
    # Package payloads precede the bundle manifest, so offsets are independent
    # of the XML's final length. All inner packages use stored ZIP members.
    bundle = ET.Element('Bundle', xmlns=BUNDLE_NS, SchemaVersion='5.0')
    ET.SubElement(bundle, 'Identity', Name='Extract.StaticFixture', Publisher='CN=Extract', Version='1.2.3.4')
    listing = ET.SubElement(bundle, 'Packages')
    offset = 0
    for name, (body, arch, resource) in packages.items():
        data_offset = offset + 30 + len(name.encode())
        ET.SubElement(listing, 'Package', Type='resource' if resource else 'application', Version='1.2.3.4',
                      Architecture=arch, ResourceId='lang-zh' if resource else '', FileName=name,
                      Offset=str(data_offset), Size=str(len(body)))
        offset = data_offset + len(body)
    if missing:
        ET.SubElement(listing, 'Package', Type='application', Version='1.2.3.4', Architecture='arm64',
                      FileName='External-arm64.msix', Offset='0', Size='1234')
    if change: change(bundle)
    name = 'AppxMetadata/AppxBundleManifest.xml'
    manifest_body = ET.tostring(bundle)
    blockmap = ET.Element('BlockMap', xmlns=BLOCK_NS, HashMethod=HASH_URIS['sha256'])
    file = ET.SubElement(blockmap, 'File', Name=name.replace('/', '\\'), Size=str(len(manifest_body)), LfhSize=str(30 + len(name)))
    for start in range(0, len(manifest_body), 65536):
        ET.SubElement(file, 'Block', Hash=base64.b64encode(hashlib.sha256(manifest_body[start:start + 65536]).digest()).decode())
    return zipped({**{n: p[0] for n, p in packages.items()}, name: manifest_body, 'AppxBlockMap.xml': ET.tostring(blockmap), '[Content_Types].xml': TYPES})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--work-root', required=True, type=Path)
    parser.add_argument('--makeappx', type=Path)
    args = parser.parse_args()
    candidates = list((Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits/10/bin').glob('*/x64/makeappx.exe'))
    packer = args.makeappx or (max(candidates, key=lambda p: tuple(map(int, p.parent.parent.name.split('.')))) if candidates else None)
    if not packer or not packer.is_file():
        print('SKIP: Windows SDK MakeAppx required'); return 77
    exe = args.executable.resolve(); root = args.work_root.resolve() / uuid.uuid4().hex; root.mkdir(parents=True)
    cache = root / 'cache'; cache.mkdir(); environment = dict(os.environ, TEMP=str(cache), TMP=str(cache))
    results = []

    def check(value, message):
        if not value: raise AssertionError(message)

    def pack(command, *arguments):
        result = subprocess.run([packer, command, *map(str, arguments), '/o'], capture_output=True, timeout=120)
        (root / f'makeappx-{uuid.uuid4().hex}.log').write_bytes(result.stdout + result.stderr)
        check(result.returncode == 0, f'MakeAppx failed: {result.stdout!r} {result.stderr!r}')

    def run(name, body, code=0, message=None, expected=None):
        path = root / name; path.write_bytes(body)
        output = root / ('output-' + uuid.uuid4().hex); output.mkdir()
        result = subprocess.run([exe, '--quiet', '--output', output, path], capture_output=True, timeout=180, env=environment)
        check(result.returncode == code, f'{name}: expected {code}, got {result.returncode}: {result.stderr!r} {result.stdout!r}')
        check(path.read_bytes() == body, 'input modified')
        check(not list(cache.iterdir()), 'temporary cache leaked')
        if message: check(message in (result.stdout + result.stderr).decode('utf-8'), f'{name}: missing diagnostic')
        results.append({'name': name, 'exitCode': code, 'sha256': hashlib.sha256(body).hexdigest()})
        if code not in (0, 299):
            check(not list(output.iterdir()), 'failed package left committed output'); return
        targets = list(output.iterdir()); check(len(targets) == 1, 'single output folder')
        target = targets[0]; report = json.loads((target / '_extract-report.json').read_text(encoding='utf-8'))
        reference = zipfile.ZipFile(io.BytesIO(body))
        for f in report['files']:
            actual = (target / f['path']).read_bytes()
            check(actual == reference.read(f['sourceExpression']), 'content mismatch against independent ZIP reader')
            check(hashlib.sha256(actual).hexdigest() == f['sha256'] and f['sourceHashVerified'], 'missing file hash/CRC')
            if f['blockHashAlgorithm']: check(f['blockHashVerified'], 'block hash not verified')
        if expected:
            for name, data in expected.items(): check((target / name).read_bytes() == data, f'original content/layout mismatch {name}')
        results[-1].update(format=report['format'], files=len(report['files']), nested=[p['status'] for p in report['nestedPackages']])
        return target, report

    # A real PNG containing no program code. All EXE-named payloads are inert text.
    def chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b'\x00\xff\x00\x00\xff')) + chunk(b'IEND', b'')
    known = {'Application.exe': b'inert executable fixture, never launched', 'Assets/logo.png': png,
             'VFS/ProgramFilesX64/Example/settings.txt': b'keep virtual filesystem layout',
             '中文/space % literal.txt': 'Chinese payload \U0001f600'.encode(),
             'block.dat': random.Random(731).randbytes(65536 * 2 + 123), 'empty.txt': b''}
    members = root / 'bundle-members'; members.mkdir()
    for arch, resource in [('x64', False), ('x86', False), ('arm64', False), ('neutral', True)]:
        source = root / ('source-' + arch); source.mkdir()
        files = {**known, 'AppxManifest.xml': manifest(arch, resource)}
        if resource: del files['Application.exe']
        for name, body in files.items():
            dest = source / name; dest.parent.mkdir(parents=True, exist_ok=True); dest.write_bytes(body)
        target = members / f'Fixture-{arch}.msix'
        pack('pack', '/d', source, '/p', target, '/nv')
        run(f'official-{arch}.msix', target.read_bytes(), expected=files)
        if arch == 'x64':
            for algorithm in ('SHA384', 'SHA512'):
                path = root / f'packer-{algorithm}.appx'; pack('pack', '/d', source, '/p', path, '/h', algorithm, '/nv')
                run(f'official-{algorithm}.appx', path.read_bytes(), expected=files)
            path = root / 'packer-stored.msix'; pack('pack', '/d', source, '/p', path, '/nc', '/nv')
            run('official-stored.msix', path.read_bytes(), expected=files)
    bundle = root / 'packer.msixbundle'; pack('bundle', '/d', members, '/p', bundle, '/bv', '1.2.3.4')
    target, report = run('official.msixbundle', bundle.read_bytes())
    check(len(report['nestedPackages']) == 4 and all(p['status'] == 'complete' for p in report['nestedPackages']), 'official bundle not fully expanded')
    for f in report['files']:
        if f['path'].startswith('packages/'):
            check('Language=' in f['conditions'] and 'TargetDeviceFamily' in f['conditions'], 'bundle resource/dependency relationship lost')
    for p in report['nestedPackages']:
        child = target / p['output']; child_report = json.loads((child / '_extract-report.json').read_text(encoding='utf-8'))
        check(child_report['format'] == 'MSIX/APPX' and child_report['nestedComplete'], 'inner package report')
        for f in child_report['files']:
            if f['blockHashAlgorithm']: check(f['blockHashVerified'], 'inner block hash not verified')
            original = known.get(f['path'])
            if original is not None: check((child / f['path']).read_bytes() == original, 'bundle payload differs from original')
        check(p['output'].startswith('packages/'), 'bundle architecture directory lost')
    sample = {'Application.exe': b'inert bytes', 'AppxManifest.xml': manifest(), 'VFS/Example/payload.txt': b'payload', '中文/percent % file.txt': b'Unicode'}
    valid = synthetic(sample)
    run('synthetic.appx', valid, expected=sample)
    run('renamed.zip', valid, expected=sample)
    run('encrypted.emsix', b'encrypted fixture', 50, '加密')
    mutate = lambda edit: synthetic(sample, edit)
    run('bad-block.msix', mutate(lambda r: r[0][0].set('Hash', base64.b64encode(bytes(32)).decode())), 13, '块哈希')
    run('missing-block.msix', mutate(lambda r: r[0].remove(r[0][0])), 13, '块摘要')
    run('extra-block.msix', mutate(lambda r: ET.SubElement(r[0], 'Block', Hash=base64.b64encode(bytes(32)).decode())), 13, '块数量')
    run('bad-base64.msix', mutate(lambda r: r[0][0].set('Hash', 'broken!')), 13, 'Base64')
    run('bad-lfh.msix', mutate(lambda r: r[0].set('LfhSize', '31')), 13, '本地头')
    run('bad-size.msix', mutate(lambda r: r[0].set('Size', '999')), 13, '大小')
    run('bad-compressed-size.msix', mutate(lambda r: r[0][0].set('Size', '2')), 13, 'Size')
    run('missing-file.msix', mutate(lambda r: r.remove(r[0])), 13, '未包含')
    run('duplicate-file.msix', mutate(lambda r: r.append(r[0])), 13, '重复')
    run('extra-file.msix', mutate(lambda r: ET.SubElement(r, 'File', Name='absent.txt', Size='0', LfhSize='40')), 13, '缺少文件')
    run('unknown-hash.msix', mutate(lambda r: r.set('HashMethod', 'SHA1')), 50, '哈希算法')
    # Tamper optional full-file hash separately from valid per-block hashes.
    def full_hash(root, bad=False):
        ET.SubElement(root[0], 'b4:FileHash', {'xmlns:b4':'http://schemas.microsoft.com/appx/2017/blockmap',
            'Hash':base64.b64encode(bytes(32) if bad else hashlib.sha256(sample['Application.exe']).digest()).decode()})
    run('filehash.msix', mutate(full_hash), expected=sample)
    run('bad-filehash.msix', mutate(lambda r: full_hash(r, True)), 13, 'FileHash')
    bad_manifest = manifest().replace(b'<Identity', b'<IdentityExtra').replace(b'/><Properties>', b'/><Properties>', 1)
    run('missing-identity.msix', synthetic({**sample, 'AppxManifest.xml': bad_manifest}), 13, '身份')
    run('missing-executable.msix', synthetic({**sample, 'AppxManifest.xml': manifest().replace(b'Application.exe', b'absent.exe')}), 13, '缺少文件')
    run('wrong-namespace.msix', synthetic({**sample, 'AppxManifest.xml': manifest().replace(b'<Applications>', b'<Applications xmlns="urn:wrong">')}), 13, '命名空间')
    run('empty-resource-property.msix', synthetic({**sample, 'AppxManifest.xml': manifest().replace(b'</Properties>', b'<ResourcePackage/></Properties>')}), 13, '布尔')
    run('duplicate-resource-property.msix', synthetic({**sample, 'AppxManifest.xml': manifest().replace(b'</Properties>', b'<ResourcePackage>false</ResourcePackage><ResourcePackage>true</ResourcePackage></Properties>')}), 13, '重复')
    run('non-xml.msix', synthetic({**sample, 'AppxManifest.xml': b'not XML'}), 13)
    dtd = b'<!DOCTYPE Package [<!ENTITY ext SYSTEM "file:///C:/private">]>' + manifest()
    run('dtd.msix', synthetic({**sample, 'AppxManifest.xml': dtd}), 13)
    run('traversal.msix', synthetic({**sample, '../outside.txt': b'bad'}), 5)
    bad = zipfile.ZipFile(io.BytesIO(valid)); raw = {n:bad.read(n) for n in bad.namelist()}
    run('encoded-traversal.msix', zipped({**raw, '%2e%2e/outside.txt': b'bad'}), 5)
    run('encoded-slash.msix', zipped({**raw, 'path%2Foutside.txt': b'bad'}), 5)
    run('decoded-collision.msix', zipped({**raw, '%41pplication.exe': b'other'}), 13, '重复')
    run('encoded-colon.msix', zipped({**raw, 'file%3Astream': b'bad'}), 5)
    run('file-directory-collision.msix', zipped({**raw, 'Application.exe/data.txt': b'bad'}), 13, '冲突')
    run('no-blockmap.msix', zipped({n:b for n,b in raw.items() if n != 'AppxBlockMap.xml'}), 13, '缺少文件')
    packages = {'App-x64.msix': (valid, 'x64', False)}
    run('synthetic.appxbundle', synthetic_bundle(packages))
    run('flat-missing.msixbundle', synthetic_bundle(packages, missing=True), 299, '外置 Bundle')
    run('wrong-offset.msixbundle', synthetic_bundle(packages, change=lambda r:r[1][0].set('Offset', '1')), 13, '偏移')
    run('wrong-arch.msixbundle', synthetic_bundle(packages, change=lambda r:r[1][0].set('Architecture', 'arm64')), 13, '架构')
    run('wrong-identity.msixbundle', synthetic_bundle(packages, change=lambda r:r[0].set('Name', 'Another.App')), 13, '身份')
    run('wrong-resource.msixbundle', synthetic_bundle(packages, change=lambda r:r[1][0].set('ResourceId', 'other')), 13, '身份')
    run('missing-embedded.msixbundle', synthetic_bundle(packages, change=lambda r:r[1][0].set('FileName', 'Absent.msix')), 13, '内嵌成员缺失')
    run('duplicate-member.msixbundle', synthetic_bundle(packages, change=lambda r:r[1].append(r[1][0])), 13, '重复')
    run('optional.msixbundle', synthetic_bundle(packages, change=lambda r:ET.SubElement(r, 'Optional')), 50, '扩展')
    run('unknown-member.msixbundle', synthetic_bundle(packages, change=lambda r:ET.SubElement(r[1], 'Unknown')), 50, '扩展')
    run('stub.msixbundle', synthetic_bundle(packages, change=lambda r:r[1][0].set('IsStub', 'true')), 299, '占位')
    invalid_child = mutate(lambda r:r[0][0].set('Hash', base64.b64encode(bytes(32)).decode()))
    run('bad-inner-hash.msixbundle', synthetic_bundle({'Bad.msix':(invalid_child,'x64',False)}), 299, '块哈希')
    (root / 'results.json').write_text(json.dumps({'makeappx':str(packer), 'cases':results}, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'PASS: {len(results)} MSIX cases; {root}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
