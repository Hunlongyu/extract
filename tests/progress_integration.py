"""Check real byte progress without sending desktop notifications or executing payloads."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import uuid
import zipfile

from cab_burn_integration import cab


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--work-root', required=True, type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    subprocess.run([str(probe), '--self-test'], check=True, capture_output=True, timeout=30)
    content = bytes(range(256)) * 16384

    def zip_bytes():
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('中文 & 文件.bin', content)
            archive.writestr('empty.txt', b'')
        return buffer.getvalue()

    def run(name, data, throwing=False, expected=0):
        directory = root / name
        directory.mkdir()
        source = directory / name
        source.write_bytes(data)
        output = directory / 'output'
        output.mkdir()
        process = subprocess.run([str(probe), str(source), str(output)] + (['--throw'] if throwing else []),
                                 capture_output=True, timeout=120)
        assert process.returncode == expected, (name, process.returncode, process.stderr.decode('utf-8', 'replace'))
        events = [json.loads(line) for line in process.stdout.decode('utf-8').splitlines()]
        assert all(e['total'] is None or 0 <= e['done'] <= e['total'] for e in events)
        return output, events

    payload = zip_bytes()
    output, events = run('bytes.zip', payload)
    extracting = [e for e in events if e['phase'] == 1]
    assert extracting and all(e['total'] == len(content) for e in extracting)
    assert len({e['done'] for e in extracting if 0 < e['done'] < len(content)}) > 5, 'No progress within a large file'
    assert max(e['done'] for e in extracting) == len(content), 'Reports/cache counted as extracted data'
    assert any(e['phase'] == 3 and 0 < e['done'] < len(content) for e in events), 'No hash progress'
    displayed = [e for e in events if e['extractionDone'] is not None]
    assert all(e['extractionTotal'] == len(content) for e in displayed)
    assert [e['extractionDone'] for e in displayed] == sorted(e['extractionDone'] for e in displayed), 'Per-file hash reset the file progress bar'
    assert any(e['phase'] == 3 and e['extractionDone'] == len(content) and 0 < e['done'] < len(content) for e in displayed)
    actual = next(output.rglob('中文 & 文件.bin')).read_bytes()
    assert hashlib.sha256(actual).digest() == hashlib.sha256(content).digest()

    inner = payload
    outer = cab([('inner.zip', inner), ('extra.txt', b'extra')], 1)
    output, events = run('nested.cab', outer)
    packages = {}
    for event in events:
        if event['phase'] == 1:
            packages.setdefault(Path(event['package']).name, []).append(event)
    assert set(packages) == {'nested.cab', 'inner.zip'}
    for name, expected in [('nested.cab', len(inner) + 5), ('inner.zip', len(content))]:
        assert all(e['total'] == expected for e in packages[name]), 'Nested totals mixed'
        assert max(e['done'] for e in packages[name]) == expected
        counters = [e for e in events if Path(e['package']).name == name and e['extractionDone'] is not None]
        assert all(e['extractionTotal'] == expected for e in counters), 'Nested display counter inherited outer bytes'
    assert len(list(output.rglob('_extract-report.json'))) == 2

    output, events = run('observer-failure.zip', payload, throwing=True)
    assert not events and next(output.rglob('中文 & 文件.bin')).read_bytes() == content

    # A valid directory with truncated/corrupt compressed data must not become a completed extraction.
    damaged = bytearray(payload)
    offset = 30 + int.from_bytes(damaged[26:28], 'little') + int.from_bytes(damaged[28:30], 'little')
    damaged[offset:offset + 8] = b'\xff' * 8
    output, events = run('corrupt.zip', damaged, expected=13)
    assert not list(output.iterdir()), 'Failed extraction left committed output'
    assert not any(e['phase'] == 1 and e['total'] and e['done'] == e['total'] for e in events)
    print('PASS: progress scope isolation, real ZIP/CAB bytes, nested packages, observer failure, corrupt input')


if __name__ == '__main__':
    main()
