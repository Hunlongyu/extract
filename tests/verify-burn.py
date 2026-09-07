"""Validate a real Burn bundle statically against its manifest and output hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def digest(path, algorithm='sha256'):
    checksum = hashlib.new(algorithm)
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            checksum.update(data)
    return checksum.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output-parent', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    source = args.input.resolve()
    parent = args.output_parent.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    before = set(parent.iterdir())
    source_hash = digest(source)
    process = subprocess.run([str(args.executable.resolve()), '--quiet', '--output', str(parent), str(source)],
                             capture_output=True, timeout=600)
    assert process.returncode in (0, 299), (process.returncode, process.stderr)
    created = set(parent.iterdir()) - before
    assert len(created) == 1
    output = created.pop()
    root = json.loads((output / '_extract-report.json').read_text('utf-8'))
    assert root['format'] == 'WiX Burn'
    manifest = ET.parse(output / 'bootstrapper/manifest.xml').getroot()
    ns = {'b': manifest.tag.split('}')[0][1:]}
    mapped = {('0', 'bootstrapper/manifest.xml')}
    for ux in manifest.findall('b:UX/b:Payload', ns):
        mapped.add((ux.attrib['SourcePath'], 'bootstrapper/' + ux.attrib['FilePath'].replace('\\', '/')))
    verified_payloads = 0
    missing_payloads = []
    for payload in manifest.findall('b:Payload', ns):
        p = payload.attrib
        folder = p['Container'] if p['Packaging'] == 'embedded' else 'external'
        path = 'packages/' + folder + '/' + p['FilePath'].replace('\\', '/')
        matches = [e for e in root['files'] if e['originalPath'] == path and e['sourceExpression'] == p['SourcePath']]
        if not matches:
            assert p['Packaging'] == 'external', p
            assert not (source.parent / p['SourcePath']).exists(), p
            missing_payloads.append(p['Id'])
            continue
        assert len(matches) == 1, p
        entry = matches[0]
        actual = output / entry['path']
        assert actual.stat().st_size == int(p['FileSize'])
        if 'Hash' in p:
            algorithm = {40: 'sha1', 64: 'sha256', 128: 'sha512'}[len(p['Hash'])]
            assert digest(actual, algorithm) == p['Hash'].lower(), p
            assert entry['sourceHashVerified']
        mapped.add((p['SourcePath'], path))
        verified_payloads += 1
    assert mapped == {(e['sourceExpression'], e['originalPath']) for e in root['files']}
    assert root['contentComplete'] == (not missing_payloads)
    layers = []
    for path in sorted(output.rglob('_extract-report.json')):
        report = json.loads(path.read_text('utf-8'))
        for entry in report['files']:
            file = path.parent / entry['path']
            assert file.stat().st_size == entry['size'] and digest(file) == entry['sha256'], file
        layers.append({'directory': str(path.parent), 'format': report['format'], 'status': report['status'],
                       'files': len(report['files']), 'bytes': report['totalBytes']})
    assert sum(layer['files'] for layer in layers) == root['treeFileCount']
    assert sum(layer['bytes'] for layer in layers) == root['treeTotalBytes']
    assert all(child['status'] == 'complete' for child in root['nestedPackages']), root['nestedPackages']
    assert digest(source) == source_hash
    result = {'input': str(source), 'inputSha256': source_hash, 'exitCode': process.returncode,
              'output': str(output), 'manifestPayloadsVerified': verified_payloads,
              'missingExternalPayloads': missing_payloads, 'nestedPackages': len(root['nestedPackages']),
              'totalFiles': root['treeFileCount'], 'totalBytes': root['treeTotalBytes'], 'layers': layers}
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'layers'}, ensure_ascii=True), flush=True)


if __name__ == '__main__':
    main()
