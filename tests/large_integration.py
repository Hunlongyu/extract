"""Large ordinary packages; run Extract only, never a payload or installer."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import uuid
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    parser.add_argument('--large-output', action='store_true', help='also write and verify >8 GiB of output')
    args = parser.parse_args()
    parent = args.work_root.resolve()
    root = parent / uuid.uuid4().hex
    root.mkdir(parents=True)
    exe = args.executable.resolve()
    results = []

    def extract(path, expected_size, expected_hash=None, expected_count=1):
        result = subprocess.run([str(exe), '--quiet', str(path)], capture_output=True, timeout=600)
        assert result.returncode == 0, (path.name, result.returncode, result.stderr)
        output = path.with_name(path.stem + '_extracted')
        report = json.loads((output / '_extract-report.json').read_text(encoding='utf-8'))
        assert len(report['files']) == expected_count
        assert sum(x['size'] for x in report['files']) == expected_size
        if expected_hash:
            payload = output / 'payload.bin'
            assert payload.stat().st_size == expected_size
            with payload.open('rb') as file:
                actual = hashlib.file_digest(file, 'sha256').hexdigest()
            assert actual == expected_hash == report['files'][0]['sha256']
        else:
            assert sum(1 for p in output.iterdir() if p.name.startswith('file-')) == expected_count
        results.append({'package': path.name, 'inputBytes': path.stat().st_size,
                        'outputBytes': expected_size, 'files': expected_count, 'exitCode': result.returncode})

    try:
        # Build and hash incrementally: the test itself must not allocate 512 MiB.
        block = bytes(range(256)) * 4096
        digest = hashlib.sha256()
        path = root / 'over-512-MiB.zip'
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as archive:
            with archive.open('payload.bin', 'w', force_zip64=True) as stream:
                for _ in range(513):
                    stream.write(block)
                    digest.update(block)
        extract(path, 513 * len(block), digest.hexdigest())

        path = root / 'over-10000-files.zip'
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as archive:
            for i in range(10001):
                archive.writestr(f'file-{i:05}.txt', b'')
        extract(path, 0, expected_count=10001)

        if args.large_output:
            path = root / 'over-8-GiB.zip'
            digest = hashlib.sha256()
            # Extra 1 MiB crosses the former output ceiling; ZIP64 lengths are used.
            blocks = 8193
            with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
                with archive.open('payload.bin', 'w', force_zip64=True) as stream:
                    for _ in range(blocks):
                        stream.write(block)
                        digest.update(block)
            extract(path, blocks * len(block), digest.hexdigest())
        evidence = parent / ('validation-' + root.name + '.json')
        evidence.write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
        print(f'PASS: {evidence}', flush=True)
    finally:
        # Only remove the fresh, direct child created by this test, after validation.
        resolved = root.resolve()
        assert resolved.parent == parent and resolved.name == root.name
        shutil.rmtree(resolved)


if __name__ == '__main__':
    main()
