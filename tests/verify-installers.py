"""Validate each user installer without executing any installer or extracted program."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

from inno_integration import Package as Inno


def digest(path, algorithm='sha256'):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--input-directory', required=True, type=Path)
    parser.add_argument('--output-parent', required=True, type=Path)
    parser.add_argument('--sevenzip', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    exe = args.executable.resolve()
    parent = args.output_parent.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    sources = sorted(p for p in args.input_directory.resolve().glob('*.exe') if p.name.lower() != 'extract.exe')
    results = []
    for source in sources:
        before_hash = digest(source)
        before = set(parent.iterdir())
        process = subprocess.run([str(exe), '--quiet', '--output', str(parent), str(source)], capture_output=True, timeout=600)
        assert process.returncode in (0, 299), (source.name, process.returncode, process.stderr)
        created = set(parent.iterdir()) - before
        assert len(created) == 1
        output = created.pop()
        root = json.loads((output / '_extract-report.json').read_text(encoding='utf-8'))
        assert root['status'] == ('complete' if process.returncode == 0 else 'partial')
        primary_output = process.stdout.decode('utf-8').splitlines()[0].partition('：')[2]
        assert Path(primary_output).is_dir(), primary_output
        layers = []
        for path in sorted(output.rglob('_extract-report.json')):
            report = json.loads(path.read_text(encoding='utf-8'))
            layer = path.parent
            for entry in report['files']:
                file = layer / entry['path']
                assert file.stat().st_size == entry['size']
                assert digest(file) == entry['sha256']
            if report['format'] == '7z':
                archive = Path(report['input'])
                check = subprocess.run([str(args.sevenzip.resolve()), 't', str(archive), '-bso0', '-bse1'], capture_output=True, timeout=300)
                assert check.returncode == 0, check.stderr
                listing = subprocess.run([str(args.sevenzip.resolve()), 'l', '-slt', '-sccUTF-8', str(archive)], capture_output=True, timeout=60)
                assert listing.returncode == 0
                body = listing.stdout.decode('utf-8').replace('\r', '').split('----------\n', 1)[1]
                expected = {}
                for block in body.split('\n\n'):
                    fields = dict(line.split(' = ', 1) for line in block.splitlines() if ' = ' in line)
                    if not fields or fields.get('Folder') == '+' or fields.get('Attributes', '').startswith('D'): continue
                    expected[fields['Path'].replace('\\', '/')] = (int(fields['Size']), fields.get('CRC', ''))
                observed = set()
                for entry in report['files']:
                    original = entry['originalPath']
                    size, crc = expected[original]
                    file = layer / entry['path']
                    actual_crc = 0
                    with file.open('rb') as stream:
                        while chunk := stream.read(1024 * 1024): actual_crc = zlib.crc32(chunk, actual_crc)
                    assert size == entry['size'] and (not crc or f'{actual_crc:08X}' == crc)
                    observed.add(original)
                assert observed == set(expected)
            if report['format'] == 'Inno Setup':
                package = Inno(Path(report['input']))
                expected = set()
                width = 20 if package.legacy else 32
                for offset in range(0, len(package.blocks[1]), package.location_size):
                    size = struct.unpack_from('<Q', package.blocks[1], offset + package.size_offset)[0]
                    checksum = package.blocks[1][offset + package.hash_offset:offset + package.hash_offset + width].hex()
                    expected.add((size, checksum))
                observed = {(entry['size'], digest(layer / entry['path'], 'sha1' if package.legacy else 'sha256')) for entry in report['files']}
                assert observed == expected
            layers.append({'directory': str(layer), 'format': report['format'], 'files': len(report['files']),
                           'bytes': report['totalBytes'], 'status': report['status'],
                           'unresolved': [e['path'] for e in report['files'] if e['path'].startswith('_unresolved/')]})
        assert digest(source) == before_hash
        result = {'name': source.name, 'inputSha256': before_hash, 'exitCode': process.returncode,
                  'output': str(output), 'primaryOutput': primary_output,
                  'totalFiles': root['treeFileCount'], 'totalBytes': root['treeTotalBytes'], 'layers': layers}
        results.append(result)
        args.report.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=True), flush=True)


if __name__ == '__main__':
    main()
