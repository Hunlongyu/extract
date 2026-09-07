"""核对用户 XnView 的 NSIS 外壳与 Inno 应用内容；不运行内嵌 EXE/DLL。"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib
from inno_integration import Package as Inno


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output-parent', required=True, type=Path)
    args = parser.parse_args()
    source = args.input.resolve()
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    assert digest == 'c497589599950fbe7ee034d51d0890b5c8c3b474f927267bddca89ed6562856a'
    start = 204288
    flags, _, _, _, _, header_size, archive_size = struct.unpack_from('<7I', data, start)
    assert flags == 2 and header_size == 9312 and start + archive_size == len(data)
    assert zlib.crc32(data[512:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0]
    assert struct.unpack_from('<I', data, start + 28)[0] == header_size
    header = data[start + 32:start + 32 + header_size]
    entries, count = struct.unpack_from('<II', header, 20)
    expected = {}
    base = start + 32 + header_size
    for i in range(count):
        op, _, _, offset, _, _, _ = struct.unpack_from('<7i', header, entries + i * 28)
        if op != 20:
            continue
        size = struct.unpack_from('<I', data, base + offset)[0]
        assert size < 0x80000000  # This sample stores all three distinct payloads verbatim.
        expected[f'nsis-{i}'] = data[base + offset + 4:base + offset + 4 + size]
    output = args.output_parent.resolve()
    output.mkdir(parents=True, exist_ok=True)
    before = set(output.iterdir())
    result = subprocess.run([str(args.executable.resolve()), '--quiet', '--output', str(output), str(source)],
                            capture_output=True, timeout=120)
    assert result.returncode == 0, result.stderr.decode('utf-8', errors='replace')
    created = set(output.iterdir()) - before
    assert len(created) == 1
    destination = created.pop()
    report = json.loads((destination / '_extract-report.json').read_text(encoding='utf-8'))
    assert report['format'] == 'NSIS' and report['formatVersion'] == '3.12'
    assert report['status'] == 'complete' and report['packageCrcVerified']
    assert len(report['files']) == 3 and report['totalBytes'] == 60469400
    for entry in report['files']:
        actual = (destination / entry['path']).read_bytes()
        assert actual == expected[entry['id']]
        assert entry['size'] == len(actual)
        assert entry['sha256'] == hashlib.sha256(actual).hexdigest()
        assert not entry['sourceHashVerified']
    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
    assert report['nestedScanned'] and report['nestedComplete']
    assert report['treeFileCount'] == 1083 and report['treeTotalBytes'] == 251326796
    assert len(report['nestedPackages']) == 1
    nested = report['nestedPackages'][0]
    assert nested['status'] == 'complete' and nested['format'] == 'Inno Setup'
    inner = Inno(destination / nested['input'])
    assert inner.version650
    inner_output = destination / nested['output']
    inner_report = json.loads((inner_output / '_extract-report.json').read_text(encoding='utf-8'))
    assert inner_report['formatVersion'] == '6.5.0' and inner_report['status'] == 'complete'
    assert len(inner_report['files']) == 1080 and inner_report['totalBytes'] == 190857396
    # 独立读取原包位置表的大小/摘要；同一位置可被多个 File 条目引用。
    expected_inner = set()
    for offset in range(0, len(inner.blocks[1]), inner.location_size):
        size = struct.unpack_from('<Q', inner.blocks[1], offset + inner.size_offset)[0]
        digest_bytes = inner.blocks[1][offset + inner.hash_offset:offset + inner.hash_offset + 32]
        expected_inner.add((size, digest_bytes.hex()))
    observed_inner = set()
    for entry in inner_report['files']:
        actual = (inner_output / entry['path']).read_bytes()
        checksum = hashlib.sha256(actual).hexdigest()
        assert len(actual) == entry['size'] and checksum == entry['sha256']
        assert entry['sourceHashAlgorithm'] == 'SHA-256' and entry['sourceHashVerified']
        observed_inner.add((len(actual), checksum))
    assert observed_inner == expected_inner
    app = inner_output / 'app/xnviewmp.exe'
    assert app.stat().st_size == 14398288
    assert hashlib.sha256(app.read_bytes()).hexdigest() == '22cb8b9c710257e52ab5d4d7a5b43c5758bd187302375a7376250068703b740e'
    assert str(app.parent) in result.stdout.decode('utf-8').splitlines()[0]
    evidence = {'inputSha256': digest, 'layer': 'NSIS + Inno 6.5.0 application files', 'output': str(destination),
                'application': str(app), 'innerFileCount': 1080, 'innerSourceSha256Verified': True,
                'treeFileCount': 1083, 'treeTotalBytes': 251326796,
                'files': [{key: f[key] for key in ('path', 'size', 'sha256')} for f in report['files']]}
    print(json.dumps(evidence, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    main()
