"""Exercise rotation, retention, fallback and real extraction diagnostics without running installers."""
import argparse
import concurrent.futures
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid
import zipfile
from cab_burn_integration import cab, bundle

parser = argparse.ArgumentParser()
parser.add_argument('--probe', type=Path, required=True)
parser.add_argument('--executable', type=Path, required=True)
parser.add_argument('--work-root', type=Path, required=True)
args = parser.parse_args()
root = args.work_root.resolve() / str(uuid.uuid4())
root.mkdir(parents=True)
LIMIT = 256 * 1024


def run_probe(primary, fallback, *, lines=1, total=16 * LIMIT, count=200, mode='normal', pause=0, wait=True):
    command = [str(args.probe.resolve()), str(primary), str(fallback), str(LIMIT), str(total), str(count),
               '30', str(lines), mode, str(pause)]
    if not wait:
        return subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=60)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def logs(directory):
    return sorted(p for p in directory.glob('extract-*.log') if p.is_file())


def records(directory):
    return [json.loads(line) for p in logs(directory) for line in p.read_text(encoding='utf-8').splitlines()]


def owned(directory, index):
    return directory / f'extract-20000101-000000-{str(uuid.UUID(int=index)).upper()}-00001.log'


# Rotation creates valid UTF-8 JSON lines, retains context, and never exceeds the per-file limit.
rotation = root / 'rotation'
run_probe(rotation, root / 'unused', lines=110)
assert len(logs(rotation)) >= 4
assert all(p.stat().st_size <= LIMIT for p in logs(rotation))
data = records(rotation)
assert len([r for r in data if r['event'] == 'test.record']) == 110
assert any(r['event'] == 'test.escape' and '中文 "引号" \\路径\r\n伪造 ERROR\t\u2028' == r['message'] for r in data)
assert any(r['event'] == 'test.after_builder' for r in data)
assert any(r['event'] == 'test.truncate' and r['message'].endswith('[truncated]') for r in data)
assert any(r['event'] == 'test.failure' and 'nativeCode=23' in r['message'] for r in data)
assert any(r['event'] == 'test.record' and r['package'].endswith('测试安装包.exe') and r['file'].endswith('内容.txt') for r in data)
assert data[-1]['event'] == 'session.end' and 'exitCode=13' in data[-1]['message']

# Total size and file count bounded independently; another application's files remain untouched.
bounded = root / 'bounded'
bounded.mkdir()
(bounded / 'other.log').write_text('keep')
(bounded / 'extract-arbitrary.log').write_text('keep')
run_probe(bounded, root / 'unused', lines=180, total=2 * LIMIT, count=2)
assert sum(p.stat().st_size for p in logs(bounded)) <= 2 * LIMIT + 4
assert len([p for p in logs(bounded) if len(p.name) == 70]) <= 2
assert (bounded / 'other.log').read_text() == 'keep'
assert (bounded / 'extract-arbitrary.log').read_text() == 'keep'

retention = root / 'retention'
retention.mkdir()
expired = owned(retention, 1)
recent = owned(retention, 2)
expired.write_text('expired')
recent.write_text('recent')
old = time.time() - 31 * 86400
os.utime(expired, (old, old))
run_probe(retention, root / 'unused')
assert not expired.exists() and recent.read_text() == 'recent'
# Check count policy even for many tiny files below the byte limit.
for i in range(3, 9):
    owned(retention, i).write_text('small')
run_probe(retention, root / 'unused', count=3)
assert len(logs(retention)) <= 3

# Blocked primary and both blocked must not crash the caller.
blocked = root / 'blocked'
blocked.write_text('ordinary file')
fallback = root / 'fallback'
run_probe(blocked / 'log', fallback)
assert any(r['event'] == 'log.fallback' for r in records(fallback))
result = run_probe(blocked / 'log', blocked / 'fallback')
assert 'unavailable' in result.stderr
assert blocked.read_text() == 'ordinary file'

# Real ACL denial: only a newly-created test directory is changed and its deny ACE is removed in finally.
denied = root / 'denied'
denied.mkdir()
sid_csv = subprocess.check_output(['whoami', '/user', '/fo', 'csv', '/nh'], text=True)
sid = next(csv.reader([sid_csv.strip()]))[1]
subprocess.run(['icacls', str(denied), '/deny', f'*{sid}:(OI)(CI)(W)'], capture_output=True, check=True)
try:
    denied_fallback = root / 'denied-fallback'
    run_probe(denied / 'log', denied_fallback)
    assert any(r['event'] == 'log.fallback' and 'nativeCode=5' in r['message'] for r in records(denied_fallback))
finally:
    subprocess.run(['icacls', str(denied), '/remove:d', f'*{sid}'], capture_output=True, check=True)

# A junction cannot redirect retention cleanup into an unrelated directory.
outside = root / 'junction-target'
outside.mkdir()
outside_file = owned(outside, 1)
outside_file.write_text('keep outside')
os.utime(outside_file, (old, old))
junction = root / 'junction'
subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(junction), str(outside)], capture_output=True, check=True)
try:
    junction_fallback = root / 'junction-fallback'
    run_probe(junction, junction_fallback)
    assert outside_file.read_text() == 'keep outside'
    assert any(r['event'] == 'log.fallback' for r in records(junction_fallback))
finally:
    os.rmdir(junction)  # remove the junction itself, never its target

# Losing the ability to create a new segment also triggers the AppData route.
runtime = root / 'runtime'
runtime_fallback = root / 'runtime-fallback'
run_probe(runtime, runtime_fallback, lines=45, mode='runtime-fallback')
assert any(r['event'] == 'log.fallback' for r in records(runtime_fallback))
assert any(r['event'] == 'session.end' for r in records(runtime_fallback))

# Active files cannot be deleted by another instance's cleanup.
active = root / 'active'
process = run_probe(active, root / 'unused', pause=1200, wait=False)
assert process.stdout.readline().strip()
active_files = logs(active)
assert active_files
run_probe(active, root / 'unused', lines=90, total=LIMIT, count=1)
assert all(p.exists() for p in active_files)
assert process.wait(timeout=10) == 0
run_probe(active, root / 'unused', total=LIMIT, count=1)
assert len(logs(active)) == 1

# Concurrent processes never mix sessions within a file or break a JSON line.
concurrent_directory = root / 'concurrent'
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(lambda _: run_probe(concurrent_directory, root / 'unused', lines=20), range(4)))
sessions = {r['session'] for r in records(concurrent_directory)}
assert len(sessions) == 4
for p in logs(concurrent_directory):
    assert len({json.loads(line)['session'] for line in p.read_text(encoding='utf-8').splitlines()}) == 1

# Real Extract: success, invalid package, corrupted payload, batch and --list JSON compatibility.
portable = root / 'portable'
portable.mkdir()
executable = portable / 'Extract.exe'
shutil.copy2(args.executable, executable)
inputs = root / 'inputs'
inputs.mkdir()
good = inputs / '中文 应用.zip'
with zipfile.ZipFile(good, 'w', compression=zipfile.ZIP_STORED) as archive:
    archive.writestr('app/内容.txt', b'KNOWN PAYLOAD')
bad = inputs / 'broken.exe'
bad.write_bytes(b'unsupported example')
corrupt = inputs / 'corrupt.zip'
corrupt.write_bytes(good.read_bytes().replace(b'KNOWN PAYLOAD', b'BROKE PAYLOAD'))


def extract(*arguments):
    before = set(logs(portable / 'log'))
    result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, timeout=30)
    created = set(logs(portable / 'log')) - before
    assert created
    entries = [json.loads(line) for path in created for line in path.read_text(encoding='utf-8').splitlines()]
    return result, entries


result, entries = extract('--quiet', good, bad, corrupt)
assert result.returncode == 299
assert any(r['event'] == 'session.start' and 'fileLimit=2097152' in r['message'] and 'directoryLimit=10485760' in r['message'] for r in entries)
assert any(r['event'] == 'input.failed' and r['package'] == str(bad) for r in entries)
assert any(r['event'] == 'input.failed' and 'CRC-32' in r['message'] and r['package'] == str(corrupt) for r in entries)
assert any(r['event'] == 'file.verified' for r in entries)
assert any(r['event'] == 'step.aborted' and r['stage'] == 'file.extract' and r['file'].endswith('内容.txt') for r in entries)
assert any(r['event'] == 'output.rollback' for r in entries)
job = next(r['message'].split('; directory=', 1)[1] for r in entries if r['event'] == 'job.created')
assert '详细日志：' in (Path(job) / 'summary.txt').read_text(encoding='utf-8')
result, entries = extract('--list', good)
assert result.returncode == 0 and json.loads(result.stdout)['format'] == 'ZIP'

# A corrupted inner archive retains both parent and child paths, and reports partial completion.
nested = inputs / 'nested.cab'
nested.write_bytes(cab([('nested/corrupt.zip', corrupt.read_bytes())]))
result, entries = extract('--quiet', nested)
assert result.returncode == 299
assert any(r['event'] == 'nested.failed' and r['rootPackage'] == str(nested) and
           r['package'].endswith('corrupt.zip') and 'CRC-32' in r['message'] for r in entries)
assert any(r['event'] == 'extraction.end' and 'status=partial' in r['message'] for r in entries)

# An offline Burn bundle's unavailable payload is a warning with the exact missing path.
burn = inputs / 'missing-payload.exe'
burn_data, *_ = bundle(executable, external_payload=True)
burn.write_bytes(burn_data)
result, entries = extract('--quiet', burn)
assert result.returncode == 299
assert any(r['event'] == 'burn.payload_missing' and r['package'] == str(burn) and r['message'].endswith('side.txt') for r in entries)

# Verify the shipping defaults, including cleaning an existing directory larger than 10 MiB.
for i in range(100, 114):
    path = owned(portable / 'log', i)
    path.write_bytes(b'old\n' * (1024 * 1024 // 4))
    previous = time.time() - 86400
    os.utime(path, (previous, previous))
result, entries = extract('--list', good)
assert result.returncode == 0
assert sum(p.stat().st_size for p in logs(portable / 'log')) <= 10 * 1024 * 1024

# Default fallback must use the current user's known LocalAppData path, not the working directory.
fallback_portable = root / 'fallback-portable'
fallback_portable.mkdir()
shutil.copy2(executable, fallback_portable / 'Extract.exe')
(fallback_portable / 'log').write_text('blocked')
local_logs = Path(os.environ['LOCALAPPDATA']) / 'Extract' / 'log'
previous_logs = set(logs(local_logs))
process = subprocess.Popen([str(fallback_portable / 'Extract.exe'), '--quiet', str(good)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
stdout, stderr = process.communicate(timeout=30)
assert process.returncode == 0, stderr
new_entries = [json.loads(line) for p in set(logs(local_logs)) - previous_logs for line in p.read_text(encoding='utf-8').splitlines()]
job = next(r['message'].split('; directory=', 1)[1] for r in new_entries if r['pid'] == process.pid and r['event'] == 'job.created')
summary = (Path(job) / 'summary.txt').read_text(encoding='utf-8')
assert str(Path(os.environ['LOCALAPPDATA']) / 'Extract' / 'log') in summary
pattern = summary.split('详细日志：', 1)[1].strip()
matches = list(Path(pattern).parent.glob(Path(pattern).name))
assert matches and any(json.loads(line)['event'] == 'log.fallback' for p in matches for line in p.read_text(encoding='utf-8').splitlines())
print(f'Logging policies and extraction diagnostics passed: {root}')
