"""Test compact/original layouts with inert NSIS fixtures; never run payloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zipfile

from worker_integration import k32, process_handle, wait_for


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    if not args.compiler.is_file():
        return 77
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    exe = root / 'Extract.exe'
    shutil.copy2(args.executable, exe)
    compiler = args.compiler.resolve()
    cache = root / 'cache'; cache.mkdir()
    env = dict(os.environ, TEMP=str(cache), TMP=str(cache))
    source = {'program.bin': b'inert application, never executed', 'settings.dat': b'configuration',
              'resource.txt': '中文资源'.encode(), 'license.dat': b'inert fixture license'}
    for name, content in source.items():
        (root / name).write_bytes(content)
    sha = lambda data: hashlib.sha256(data).hexdigest()
    cases = []

    def build(name, body, stored=True):
        script = root / (name + '.nsi')
        script.write_text(f'Unicode true\nName "Layout fixture"\nOutFile "{name}.exe"\nSetCompressor zlib\n'
                          + ('SetCompress off\n' if stored else '') + 'Section\n' + body + '\nSectionEnd\n', encoding='utf-8-sig')
        result = subprocess.run([compiler, '/V2', script], capture_output=True, timeout=90)
        assert result.returncode == 0, (result.stdout, result.stderr)
        return root / (name + '.exe')

    def run(package, layout=None, code=0, target=None):
        target = target or root / ('result-' + uuid.uuid4().hex)
        target.mkdir(exist_ok=True)
        before = set(target.iterdir())
        digest = sha(package.read_bytes())
        options = ['--layout', layout] if layout else []
        process = subprocess.run([exe, '--quiet', *options, '--output', target, package], capture_output=True, timeout=120, env=env)
        assert process.returncode == code, (package, process.returncode, process.stderr.decode('utf-8', errors='replace'))
        assert sha(package.read_bytes()) == digest
        assert not list(cache.iterdir()), 'temporary codec files leaked'
        assert not list(target.glob('.extract-*.tmp')), 'layout staging leaked'
        outputs = set(target.iterdir()) - before
        assert len(outputs) == 1
        output = outputs.pop()
        report = json.loads((output / '_extract-report.json').read_text(encoding='utf-8'))
        assert report['status'] == ('complete' if code == 0 else 'partial')
        files = {}
        for file in report['files']:
            assert '..' not in Path(file['path']).parts
            content = (output / file['path']).read_bytes()
            assert len(content) == file['size'] and sha(content) == file['sha256']
            files[file['path']] = content
        assert sum(len(data) for data in files.values()) == report['totalBytes']
        if report['layout'] == 'compact':
            assert report['treeFileCount'] == len(files) and report['treeTotalBytes'] == report['totalBytes']
            assert report['pathBase'] == 'extraction-root'
            assert str(output) == process.stdout.decode('utf-8').splitlines()[0].split('：', 1)[1]
            for nested in report['nestedPackages']:
                assert (output / nested['input']).is_file()
                if nested['report']:
                    child = json.loads((output / nested['report']).read_text(encoding='utf-8'))
                    assert (output / nested['output']).is_dir()
                    assert child['pathBase'] == 'extraction-root'
                    for file in child['files']:
                        assert sha((output / file['path']).read_bytes()) == file['sha256']
                    if child['compiledScript']:
                        s = child['compiledScript']
                        assert sha((output / s['textPath']).read_bytes()) == s['textSha256']
                        assert sha((output / s['metadataPath']).read_bytes()) == s['metadataSha256']
        if report['compiledScript']:
            s = report['compiledScript']
            assert sha((output / s['metadataPath']).read_bytes()) == s['metadataSha256']
        cases.append({'input': package.name, 'layout': layout or 'default', 'files': len(files), 'status': report['status']})
        return output, report, files

    app_body = r'''SetOutPath "$INSTDIR"
File /oname=Main.exe "program.bin"
File /oname=Settings.ini "settings.dat"
SetOutPath "$INSTDIR\Languages\Flags"
File "resource.txt"
'''
    app = build('single-app', app_body)
    output, report, files = run(app)
    assert set(files) == {'Main.exe', 'Settings.ini', 'Languages/Flags/resource.txt'}
    assert all(f['originalPath'].startswith('app/') for f in report['files'])
    _, original, original_files = run(app, 'original')
    assert {p.removeprefix('app/'): value for p, value in original_files.items()} == files
    assert original['layout'] == 'static logical directories'
    _, explicit, explicit_files = run(app, 'compact')
    assert explicit_files == files
    listing = subprocess.run([exe, '--list', app], capture_output=True, timeout=20, env=env)
    assert listing.returncode == 0
    listing = json.loads(listing.stdout)
    assert listing['requestedLayout'] == 'compact' and all(f['path'].startswith('app/') for f in listing['files'])

    shell = build('shell-wrapper', r'''SetShellVarContext all
SetOutPath "$DOCUMENTS\Vendor\App"
File /oname=License.key "license.dat"
SetShellVarContext current
SetOutPath "$APPDATA\WrappedApp"
File /oname=Main.exe "program.bin"
SetOutPath "$APPDATA\WrappedApp\Languages"
File "resource.txt"
ExecWait '"$APPDATA\WrappedApp\Main.exe"'
''')
    _, report, files = run(shell)
    assert set(files) == {'Main.exe', 'Languages/resource.txt', '_extra/PublicDocuments/License.key'}
    assert next(f for f in report['files'] if f['path'].endswith('License.key'))['sourceExpression'].startswith('$DOCUMENTS[all]')

    ambiguous = build('ambiguous', r'''SetOutPath "$APPDATA\One"
File /oname=One.exe "program.bin"
Exec "$APPDATA\One\One.exe"
SetOutPath "$LOCALAPPDATA\Two"
File /oname=Two.exe "program.bin"
Exec "$LOCALAPPDATA\Two\Two.exe"
''')
    _, _, files = run(ambiguous)
    assert set(files) == {'AppData/Roaming/One/One.exe', 'AppData/Local/Two/Two.exe'}
    partial = build('unknown', app_body + 'ReadRegStr $OUTDIR HKCU "Software\\Fixture" "unknown"\nFile "resource.txt"')
    _, _, files = run(partial, code=299)
    assert 'app/Main.exe' in files and any(p.startswith('_unresolved/') for p in files)
    collision = build('reserved', app_body + 'SetOutPath "$INSTDIR\\_extra"\nFile "resource.txt"')
    _, _, files = run(collision)
    assert 'app/Main.exe' in files and 'app/_extra/resource.txt' in files

    middle = build('middle', 'SetOutPath "$PLUGINSDIR"\nFile "single-app.exe"')
    outer = build('outer', 'SetOutPath "$PLUGINSDIR"\nFile "middle.exe"')
    _, report, files = run(outer)
    assert 'Main.exe' in files and len(files) == 5 and len(report['nestedPackages']) == 2
    assert all('_extracted/' not in name for name in files)
    assert len({n['report'] for n in report['nestedPackages']}) == 2
    sibling = build('sibling', app_body.replace('Main.exe', 'Other.exe'))
    multiple = build('multiple', 'SetOutPath "$PLUGINSDIR"\nFile "single-app.exe"\nFile "sibling.exe"')
    _, _, files = run(multiple)
    assert len(files) == 8 and any(p.startswith('_packages/p1-') and p.endswith('/Main.exe') for p in files)
    assert any(p.startswith('_packages/p2-') and p.endswith('/Other.exe') for p in files)
    duplicate = build('duplicate', 'SetOutPath "$PLUGINSDIR"\nFile /oname=one.exe "single-app.exe"\nFile /oname=two.exe "single-app.exe"')
    _, report, files = run(duplicate)
    assert len(files) == 5 and len({n['report'] for n in report['nestedPackages']}) == 1

    with zipfile.ZipFile(root / 'inner.zip', 'w') as z:
        z.writestr('Program/Main.exe', source['program.bin'])
        z.writestr('Program/data/resource.txt', source['resource.txt'])
    _, _, zip_files = run(root / 'inner.zip')
    assert set(zip_files) == {'Program/Main.exe', 'Program/data/resource.txt'}
    archive_wrapper = build('archive-wrapper', 'SetOutPath "$PLUGINSDIR"\nFile "inner.zip"')
    _, _, files = run(archive_wrapper)
    assert 'Program/Main.exe' in files and len(files) == 3
    resource_wrapper = build('resource-wrapper', 'SetOutPath "$INSTDIR"\nFile /oname=uninstall.ico "resource.txt"\nSetOutPath "$PLUGINSDIR"\nFile "inner.zip"')
    _, _, files = run(resource_wrapper)
    assert 'Program/Main.exe' in files and '_extra/App/uninstall.ico' in files and len(files) == 4

    def pe(machine):
        data = bytearray(1024); data[:2] = b'MZ'; struct.pack_into('<I', data, 60, 128)
        data[128:132] = b'PE\0\0'; struct.pack_into('<H', data, 132, machine)
        struct.pack_into('<H', data, 148, 224 if machine == 0x14c else 240)
        struct.pack_into('<H', data, 152, 0x10b if machine == 0x14c else 0x20b)
        return bytes(data)
    (root / 'x64.bin').write_bytes(pe(0x8664)); (root / 'x86.bin').write_bytes(pe(0x14c))
    dual = build('dual-architecture', r'''StrCmp $0 "64" native_a native_b
native_a:
SetOutPath "$INSTDIR"
File /oname=Main.exe "x64.bin"
Goto joined
native_b:
SetOutPath "$INSTDIR"
File /oname=Main.exe "x86.bin"
joined:
SetOutPath "$INSTDIR\Languages"
File "resource.txt"
''')
    _, _, files = run(dual)
    assert set(files) == {'x64/Main.exe', 'x86/Main.exe', 'x64/Languages/resource.txt', 'x86/Languages/resource.txt'}
    repeat = root / 'repeat'; run(app, target=repeat); run(app, target=repeat)
    assert len(list(repeat.iterdir())) == 2
    for options in (['--layout'], ['--layout', 'invalid'], ['--layout', 'compact', '--layout', 'original']):
        result = subprocess.run([exe, '--quiet', *options, app], capture_output=True, timeout=20)
        assert result.returncode == 160

    # Interrupt only our Extract worker after the decoded layer is staged and
    # the second (final) output exists. No package code is ever launched.
    with (root / 'large.bin').open('wb') as file:
        file.truncate(256 * 1024 * 1024)
    slow = build('slow', 'SetOutPath "$INSTDIR"\nFile "large.bin"', stored=False)
    def event(pid, name):
        for log in (root / 'log').glob('*.log'):
            for line in log.read_text(encoding='utf-8').splitlines():
                try: value = json.loads(line)
                except json.JSONDecodeError: continue
                if value['pid'] == pid and value['event'] == name: return value
    for mode in ('cancel', 'crash'):
        target = root / mode; target.mkdir()
        process = subprocess.Popen([exe, '--quiet', '--output', target, slow], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        worker = None
        try:
            started = wait_for(lambda: event(process.pid, 'worker.started'))
            worker = process_handle(int(re.search(r'pid=(\d+)', started['message'])[1]), exe)
            job = wait_for(lambda: event(process.pid, 'job.created'))
            wait_for(lambda: len(list(target.glob('.extract-*.tmp'))) == 2, timeout=30)
            if mode == 'cancel':
                job_id = Path(job['message'].split('; directory=', 1)[1]).name
                result = subprocess.run([exe, '--cancel', job_id], capture_output=True, timeout=15)
                assert result.returncode == 0
            else:
                assert k32.TerminateProcess(worker, 0xC0000005)
            stdout, stderr = process.communicate(timeout=30)
            assert process.returncode == (1223 if mode == 'cancel' else 1067), (mode, process.returncode, stderr)
            assert not list(target.iterdir()), f'{mode}: staging leaked'
            assert not list(cache.iterdir())
            cases.append({'interrupt': mode, 'stage': 'compact finalization', 'cleanup': True})
        finally:
            if process.poll() is None: process.kill(); process.communicate()
            if worker: k32.CloseHandle(worker)
    evidence = {'cases': cases, 'count': len(cases), 'installerOrPayloadExecuted': False}
    (root / 'validation.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'fixtures': str(root), **evidence}, ensure_ascii=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
