"""Known-content CAB/Burn data; execute only Extract, never generated stub EXEs."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid
import xml.etree.ElementTree as ET
import zlib


def cab(files, method=0):
    table = bytearray()
    data = bytearray()
    for name, content in files:
        table += struct.pack('<IIHHHH', len(content), len(data), 0, 0, 0, 0x80)
        table += name.encode('utf-8') + b'\0'
        data += content
    blocks = bytearray()
    count = 0
    for offset in range(0, len(data), 32768):
        raw = data[offset:offset + 32768]
        if method == 1:
            c = zlib.compressobj(wbits=-15)
            compressed = b'CK' + c.compress(raw) + c.flush()
        else:
            compressed = raw
        blocks += struct.pack('<IHH', 0, len(compressed), len(raw)) + compressed
        count += 1
    data_offset = 44 + len(table)
    header = struct.pack('<4sIIIIIBBHHHHH', b'MSCF', 0, data_offset + len(blocks), 0, 44, 0, 3, 1, 1, len(files), 0, 1, 0)
    return header + struct.pack('<IHH', data_offset, count, method) + table + blocks


def pe_stub(exe):
    data = bytearray(exe.read_bytes())
    pe = struct.unpack_from('<I', data, 60)[0]
    count = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    section_table = pe + 24 + optional_size
    end = max(sum(struct.unpack_from('<II', data, section_table + i * 40 + 16)) for i in range(count))
    data = data[:end]
    last = section_table + (count - 1) * 40
    raw_size, raw_offset = struct.unpack_from('<II', data, last + 16)
    assert raw_size >= 64
    security = pe + 24 + (128 if struct.unpack_from('<H', data, pe + 24)[0] == 0x10b else 144)
    struct.pack_into('<II', data, security, 0, 0)
    return data, last, raw_offset, security


def bundle(exe, modern=False, attached=True, external_payload=False, original_signature=False,
           mutate=None, extra_payload=b''):
    stub, last, raw, security = pe_stub(exe)
    stub[last:last + 8] = b'.wixburn'
    contents = [('a0', b'application payload\n' * 4000), ('a1', bytes(range(256)) * 40)]
    if extra_payload:
        contents.append(('a2', extra_payload))
    payload_cab = cab(contents, 1)
    ns = 'http://wixtoolset.org/schemas/v4/2008/Burn' if modern else 'http://schemas.microsoft.com/wix/2008/Burn'
    root = ET.Element('BurnManifest', xmlns=ns)
    ux = ET.SubElement(root, 'UX')
    ET.SubElement(ux, 'Payload', Id='ba', SourcePath='u0', FilePath='Bootstrapper.dll')
    digest = hashlib.sha512 if modern else hashlib.sha1
    attributes = dict(Id='Main', FilePath='data.cab', FileSize=str(len(payload_cab)), Hash=digest(payload_cab).hexdigest())
    if attached:
        attributes.update(Attached='yes', AttachedIndex='1')
    ET.SubElement(root, 'Container', **attributes)
    names = ['中文/app.dat', 'other.bin'] + (['child.cab'] if extra_payload else [])
    for (name, data), file in zip(contents, names):
        ET.SubElement(root, 'Payload', Id=name, SourcePath=name, FilePath=file, Packaging='embedded',
                      Container='Main', FileSize=str(len(data)), Hash=digest(data).hexdigest())
    if external_payload:
        ET.SubElement(root, 'Payload', Id='external', SourcePath='side.txt', FilePath='side.txt', Packaging='external',
                      FileSize='4', Hash=digest(b'side').hexdigest())
    if mutate:
        mutate(root)
    manifest = ET.tostring(root, encoding='utf-8', xml_declaration=True)
    ux_cab = cab([('0', manifest), ('u0', b'not an executable')], 1)
    sizes = [len(ux_cab)] + ([len(payload_cab)] if attached else [])
    certificate = b'\0' * 16 if original_signature else b''
    original_offset = len(stub) + len(ux_cab) if original_signature else 0
    metadata = struct.pack('<II16sIIIIII', 0x00f14300, 2, uuid.uuid4().bytes_le, len(stub), 0,
                           original_offset, len(certificate), 1, len(sizes))
    metadata += struct.pack('<' + 'I' * len(sizes), *sizes)
    stub[raw:raw + len(metadata)] = metadata
    result = stub + ux_cab + certificate + (payload_cab if attached else b'')
    final_signature = len(result)
    result += b'\0' * 16
    struct.pack_into('<II', result, security, final_signature, 16)
    return bytes(result), payload_cab, dict(contents), raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--work-root', required=True, type=Path)
    args = parser.parse_args()
    exe = args.executable.resolve()
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    results = []

    def run(name, data, code=0, expected=None, extras=None):
        directory = root / name
        directory.mkdir()
        source = directory / (name if name.endswith(('.exe', '.cab')) else name + '.exe')
        source.write_bytes(data)
        for file, content in (extras or {}).items():
            (directory / file).write_bytes(content)
        output = directory / 'output'
        output.mkdir()
        listed = subprocess.run([str(exe), '--quiet', '--list', str(source)], capture_output=True, timeout=120)
        process = subprocess.run([str(exe), '--quiet', '--output', str(output), str(source)], capture_output=True, timeout=120)
        assert process.returncode == code, (name, code, process.returncode, process.stderr.decode('utf-8','replace'))
        assert source.read_bytes() == data, 'input changed'
        results.append({'name': name, 'exitCode': code, 'listExitCode': listed.returncode})
        if code not in (0, 299):
            assert not list(output.iterdir()), 'failed extraction committed output or left staging'
            return None
        assert listed.returncode == 0, listed.stderr
        listing = json.loads(listed.stdout)
        assert len(list(output.iterdir())) == 1
        target = next(output.iterdir())
        report = json.loads((target / '_extract-report.json').read_text(encoding='utf-8'))
        assert len(report['files']) == len(listing['files'])
        actual = {}
        for item in report['files']:
            value = (target / item['path']).read_bytes()
            assert len(value) == item['size']
            assert hashlib.sha256(value).hexdigest() == item['sha256']
            if item['sourceHashAlgorithm']:
                assert item['sourceHashVerified'], item
            actual[item['path']] = value
        if expected is not None:
            assert actual == expected, (name, list(actual))
        return report

    files = [('中文/说明.txt', '测试内容'.encode()), ('bin/data.dat', bytes(range(256))*512), ('empty', b'')]
    for method in (0, 1):
        run(f'cab-{method}.cab', cab(files, method), expected=dict(files))
    stub, _, _, _ = pe_stub(exe)
    run('cab-sfx.exe', stub + b'config\0' + cab(files, 1), expected=dict(files))
    run('cab-duplicate.cab', cab([('same.txt',b'a'),('same.txt',b'b')]))
    run('cab-prefix.cab', cab([('folder',b'a'),('folder/file',b'b')]))
    run('cab-reserved.cab', cab([('_extract-report.json',b'a')]))
    for name in ('../escape','C:\\escape','file:ads','CON.txt','trailing.','\\server\\file'):
        run('cab-unsafe-' + str(len(results)) + '.cab', cab([(name,b'x')]), 5)
    invalid = bytearray(cab(files)); struct.pack_into('<I', invalid, 8, len(invalid) + 1)
    run('cab-length.cab', invalid, 13)
    invalid = bytearray(cab(files)); struct.pack_into('<H', invalid, 30, 1)
    # Setting a volume flag without inserting its strings yields an invalid name.
    run('cab-volume.cab', invalid, 5)
    invalid = bytearray(cab(files)); struct.pack_into('<I', invalid, 16, 0xfffffff0)
    run('cab-offset.cab', invalid, 13)
    invalid = bytearray(cab([('safe',b'data')])); invalid[44+16:44+20] = b'\xff\xff\xff\xff'
    run('cab-utf8.cab', invalid, 13)
    child = cab([('inner.txt',b'inner payload')])
    report = run('cab-nested.cab', cab([('child.cab',child)]))
    assert report['treeFileCount'] == 2 and report['nestedPackages'][0]['status'] == 'complete'

    for modern in (False, True):
        for signed in (False, True):
            data, _, contents, _ = bundle(exe, modern=modern, original_signature=signed)
            report = run(f'burn-{modern}-{signed}.exe', data)
            assert report['format'] == 'WiX Burn' and report['contentComplete']
            assert report['files'][2]['sourceHashAlgorithm'] == ('SHA-512' if modern else 'SHA-1')
    data, container, _, _ = bundle(exe, modern=True, attached=False)
    run('burn-detached.exe', data, extras={'data.cab':container})
    report = run('burn-detached-missing.exe', data, 299)
    assert not report['contentComplete'] and report['notes']
    data, _, _, _ = bundle(exe, modern=True, external_payload=True)
    run('burn-external.exe', data, extras={'side.txt':b'side'})
    report = run('burn-online.exe', data, 299)
    assert not report['contentComplete'] and report['status'] == 'partial'
    run('burn-external-corrupt.exe', data, 13, extras={'side.txt':b'evil'})

    mutations = [
        ('traversal', lambda r:r.find('Payload').set('FilePath','../escape'),5),
        ('badsize',lambda r:r.find('Payload').set('FileSize','900'),13),
        ('badhash',lambda r:r.find('Payload').set('Hash','0'*40),13),
        ('badcontainerhash',lambda r:r.find('Container').set('Hash','0'*40),13),
        ('badindex',lambda r:r.find('Container').set('AttachedIndex','99'),13),
        ('missingmember',lambda r:r.find('Payload').set('SourcePath','absent'),13),
        ('duplicate',lambda r:r.append(ET.fromstring(ET.tostring(r.find('Payload')))),13),
        ('overflow',lambda r:r.find('Payload').set('FileSize','9'*60),13),
        ('unknowncontainer',lambda r:r.find('Payload').set('Container','absent'),13),
        ('unmapped',lambda r:r.remove(r.find('Payload')),13),
    ]
    for name, mutate, code in mutations:
        data, _, _, _ = bundle(exe,mutate=mutate)
        run('burn-' + name + '.exe', data, code)
    data, _, _, raw = bundle(exe)
    run('burn-truncated.exe', data[:-100], 13)
    version = bytearray(data); struct.pack_into('<I', version, raw+4, 99)
    run('burn-version.exe',version,50)
    dtd = b'<!DOCTYPE BurnManifest [<!ENTITY external SYSTEM "file:///C:/Windows/win.ini">]><BurnManifest xmlns="http://schemas.microsoft.com/wix/2008/Burn">&external;</BurnManifest>'
    ux_cab = cab([('0',dtd)],1)
    stub, last, raw, _ = pe_stub(exe); stub[last:last+8] = b'.wixburn'
    metadata = struct.pack('<II16sIIIIIII',0x00f14300,2,uuid.uuid4().bytes_le,len(stub),0,0,0,1,1,len(ux_cab))
    stub[raw:raw+len(metadata)] = metadata
    run('burn-dtd.exe',stub+ux_cab,13)
    (root/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print(f'PASS: {len(results)} CAB/Burn cases, known bytes, hashes, UTF-8, SFX, detached/online, malformed metadata and rollback. {root}')


if __name__ == '__main__':
    main()
