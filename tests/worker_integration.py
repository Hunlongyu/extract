"""Supervise only our Extract processes and inert ZIPs; never execute input/payload code."""
import argparse
import ctypes
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid
import zipfile

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
ntdll = ctypes.WinDLL('ntdll')
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.OpenProcess.restype = w.HANDLE
k32.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
k32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
k32.CloseHandle.argtypes = [w.HANDLE]
k32.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
ntdll.NtSuspendProcess.argtypes = [w.HANDLE]
ntdll.NtResumeProcess.argtypes = [w.HANDLE]


def process_handle(pid, executable):
    handle = k32.OpenProcess(0x100000 | 0x1000 | 0x800 | 1, False, pid)
    assert handle, ctypes.get_last_error()
    buffer = ctypes.create_unicode_buffer(32768)
    length = w.DWORD(len(buffer))
    assert k32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(length))
    assert Path(buffer.value).resolve() == executable.resolve(), buffer.value
    return handle


def wait_for(condition, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = condition()
        if value:
            return value
        time.sleep(.01)
    raise AssertionError('condition timed out')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    executable = root / 'Extract.exe'
    shutil.copy2(args.executable, executable)
    slow = root / 'slow.zip'
    with zipfile.ZipFile(slow, 'w', zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
        with archive.open('payload.bin', 'w') as file:
            for _ in range(1024):
                file.write(bytes(1024 * 1024))
    good = root / '正常 空格😀.zip'
    payload = '原始内容'.encode()
    with zipfile.ZipFile(good, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('中文.txt', payload)
    results = []

    def run(*arguments, expected=0):
        result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, timeout=30)
        assert result.returncode == expected, (arguments, result.returncode, result.stderr.decode('utf-8'))
        return result

    def records(pid):
        values = []
        for log in (root / 'log').glob('*.log'):
            try:
                for line in log.read_text(encoding='utf-8').splitlines():
                    try:
                        item = json.loads(line)
                        if item['pid'] == pid:
                            values.append(item)
                    except json.JSONDecodeError:
                        pass
            except FileNotFoundError:
                pass
        return values

    def event(pid, name):
        return next((r for r in records(pid) if r['event'] == name), None)

    def begin(name, extra=(), batch=False):
        directory = root / name
        directory.mkdir()
        process = subprocess.Popen([str(executable), '--quiet', '--output', str(directory), *extra,
                                    str(slow), *([str(good)] if batch else [])],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            started = wait_for(lambda: event(process.pid, 'worker.started'))
            worker = process_handle(int(re.search(r'pid=(\d+)', started['message'])[1]), executable)
            created = wait_for(lambda: event(process.pid, 'job.created'))
            job = Path(created['message'].split('; directory=', 1)[1])
            staging = wait_for(lambda: next((p for p in directory.glob('.extract-*.tmp')
                                            if (p / 'payload.bin').exists() and (p / 'payload.bin').stat().st_size >= 65536), None))
            return process, worker, job, directory, staging
        except BaseException:
            process.kill(); process.communicate(); raise

    def finish(process, worker, expected):
        try:
            stdout, stderr = process.communicate(timeout=20)
            assert process.returncode == expected, (process.returncode, stderr.decode('utf-8'))
            assert k32.WaitForSingleObject(worker, 5000) == 0, 'orphaned worker'
            return stdout, stderr
        finally:
            if process.poll() is None:
                process.kill(); process.communicate()
            k32.CloseHandle(worker)

    try:
        listed = run('--list', good)
        assert json.loads(listed.stdout)['files'][0]['path'] == '中文.txt'
        destination = root / 'normal'; destination.mkdir()
        run('--quiet', '--output', destination, good)
        assert (destination / (good.stem + '_extracted') / '中文.txt').read_bytes() == payload
        for values in [('--timeout', '-1'), ('--timeout', '4294967296'), ('--timeout', 'abc'),
                       ('--timeout', '1', '--timeout', '2')]:
            run(*values, good, expected=160)
        run('--internal-worker-v1', '0', '0', '0', expected=5)
        results.append('isolated extraction/list, Unicode, CLI validation')

        process, worker, job, directory, staging = begin('cancel', batch=True)
        run('--open-notification', 'hunlongyu-extract://cancel/' + job.name)
        finish(process, worker, 1223)
        assert not list(directory.iterdir()), 'cancel left uncommitted output or started the next input'
        summary = (job / 'summary.txt').read_text(encoding='utf-8')
        assert '已取消' in summary and '未开始 1' in summary
        assert (job / 'state.txt').read_text() == 'finished'
        results.append('protocol cancel, batch not-started count, cooperative rollback')

        process, worker, job, directory, staging = begin('crash', batch=True)
        assert k32.TerminateProcess(worker, 0xC0000005)
        finish(process, worker, 299)
        assert not list(directory.glob('.extract-*.tmp'))
        assert (directory / (good.stem + '_extracted') / '中文.txt').read_bytes() == payload
        assert '3221225477' in (job / 'summary.txt').read_text(encoding='utf-8')
        results.append('abnormal worker exit, identity cleanup, batch continuation')

        process, worker, job, directory, staging = begin('timeout', extra=('--timeout', '1'), batch=True)
        assert ntdll.NtSuspendProcess(worker) == 0
        finish(process, worker, 299)
        assert not list(directory.glob('.extract-*.tmp'))
        assert (directory / (good.stem + '_extracted') / '中文.txt').read_bytes() == payload
        assert '指定处理时限' in (job / 'summary.txt').read_text(encoding='utf-8')
        results.append('timeout force-stops suspended worker and continues batch')

        process, worker, job, directory, staging = begin('cancel-suspended')
        assert ntdll.NtSuspendProcess(worker) == 0
        run('--cancel', job.name)
        finish(process, worker, 1223)
        assert not list(directory.iterdir())
        results.append('cancel force-stop fallback')

        process, worker, job, directory, staging = begin('replacement')
        parent = process_handle(process.pid, executable)
        try:
            assert ntdll.NtSuspendProcess(parent) == 0
            assert k32.TerminateProcess(worker, 0xC0000005)
            assert k32.WaitForSingleObject(worker, 5000) == 0
            original = staging / 'payload.bin'
            original.rename(directory / 'preserved-original.bin')
            original.write_bytes(b'unrelated replacement')
            (staging / 'unregistered.txt').write_bytes(b'not created by Extract')
        finally:
            ntdll.NtResumeProcess(parent); k32.CloseHandle(parent)
        finish(process, worker, 1067)
        assert original.read_bytes() == b'unrelated replacement'
        assert (staging / 'unregistered.txt').read_bytes() == b'not created by Extract'
        results.append('replacement file identity and unregistered files preserved')

        process, worker, job, directory, staging = begin('parent-death')
        process.kill(); finish(process, worker, 1)
        assert (job / 'state.txt').read_text() == 'running'
        # Read-only listing is a new normal invocation; recover stale latest record without restarting extraction.
        run('--list', good)
        assert (job / 'state.txt').read_text() == 'interrupted'
        assert '上次任务被中断' in (job / 'summary.txt').read_text(encoding='utf-8')
        assert staging.exists(), 'recovery must not infer deletion rights from disk text'
        results.append('parent death reaps child; next invocation marks interrupted and preserves files')

        report = {'scenarios': results, 'count': len(results), 'inputSHA256': hashlib.sha256(slow.read_bytes()).hexdigest()}
        print(json.dumps(report, ensure_ascii=True), flush=True)
        (root.parent / ('validation-' + root.name + '.json')).write_text(json.dumps(report, indent=2), encoding='utf-8')
    finally:
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
