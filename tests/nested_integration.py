"""Generate inert nested packages with official compilers; never execute their payloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid

from inno_integration import Package as Inno, check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--work-root', required=True, type=Path)
    parser.add_argument('--inno-compiler', type=Path)
    parser.add_argument('--nsis-compiler', type=Path)
    args = parser.parse_args()
    programs = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)'))
    iscc = (args.inno_compiler or programs / 'Inno Setup 6/ISCC.exe').resolve()
    makensis = (args.nsis_compiler or programs / 'NSIS/Bin/makensis.exe').resolve()
    if not iscc.is_file() or not makensis.is_file():
        print('SKIP: nested fixtures require verified official ISCC.exe and makensis.exe')
        return 77
    executable = args.executable.resolve()
    root = args.work_root.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    print(f'Fixtures: {root}', flush=True)
    cache = root / 'cache'
    cache.mkdir()
    environment = dict(os.environ, TEMP=str(cache), TMP=str(cache))
    payload = b'Actual application payload\n' * 51
    (root / 'payload.bin').write_bytes(payload)
    (root / 'ordinary.exe').write_bytes(executable.read_bytes())
    runs = []

    def compile_source(name, text, compiler, suffix):
        source = root / (name + suffix)
        source.write_text(text, encoding='utf-8-sig')
        result = subprocess.run([str(compiler), '/Q' if suffix == '.iss' else '/V2', str(source)],
                                capture_output=True, timeout=90)
        check(result.returncode == 0, f'compile {name}: {result.stdout!r} {result.stderr!r}')
        return root / (name + '.exe')

    def inno(name, dynamic=False, content=payload):
        (root / 'payload.bin').write_bytes(content)
        return compile_source(name, f'''[Setup]
AppName=Extract Nested Fixture
AppVersion=1
DefaultDirName={{autopf}}\\ExtractFixture
OutputDir=.
OutputBaseFilename={name}
Compression=lzma2
LZMAUseSeparateProcess=no
[Files]
Source: "payload.bin"; DestDir: "{'{code:GetDir}' if dynamic else '{app}'}"
[Code]
function GetDir(Param: String): String;
begin Result := ExpandConstant('{{app}}'); end;
''', iscc, '.iss')

    def nsis(name, files, app=False):
        body = '\n'.join(f'File /oname={destination} "{source}"' for destination, source in files)
        return compile_source(name, f'''Unicode true
Name "Extract Nested Fixture"
OutFile "{name}.exe"
SetCompressor /SOLID lzma
Section
SetOutPath "{'$INSTDIR' if app else '$PLUGINSDIR'}"
{body}
SectionEnd
''', makensis, '.nsi')

    def report(path):
        return json.loads((path / '_extract-report.json').read_text(encoding='utf-8'))

    def run(path, code=0, timeout=90):
        target = root / ('result-' + uuid.uuid4().hex)
        target.mkdir()
        digest = hashlib.sha256(path.read_bytes()).digest()
        result = subprocess.run([str(executable), '--quiet', '--output', str(target), str(path)],
                                capture_output=True, timeout=timeout, env=environment)
        check(result.returncode == code, f'{path.name}: expected {code}, got {result.returncode}: {result.stderr!r}')
        check(not list(cache.iterdir()), 'temporary cache residue')
        check(hashlib.sha256(path.read_bytes()).digest() == digest, 'input mutated')
        outputs = list(target.iterdir())
        check(len(outputs) == 1 and '.tmp' not in outputs[0].name, 'root commit')
        output = outputs[0]
        data = report(output)
        check(data['status'] == ('complete' if code == 0 else 'partial'), 'root completion state')
        check(data['nestedScanned'], 'nested scan completed')
        for item in output.rglob('*'):
            check(not item.name.endswith('.tmp'), 'staging residue')
        runs.append({'input': path.name, 'exitCode': code, 'treeFiles': data['treeFileCount']})
        return output, data, result.stdout.decode('utf-8')

    inner = inno('inner')
    outer = nsis('outer', [('inner.exe', inner), ('ordinary.exe', root / 'ordinary.exe')])
    output, data, text = run(outer)
    check(len(data['nestedPackages']) == 1, 'ordinary application incorrectly treated as installer')
    child = output / data['nestedPackages'][0]['output']
    check((child / 'app/payload.bin').read_bytes() == payload, 'nested application content')
    check(report(child)['files'][0]['sourceHashVerified'], 'inner source SHA-256')
    check(data['treeFileCount'] == 3 and data['treeTotalBytes'] == data['totalBytes'] + len(payload), 'shared accounting')
    check(str(child / 'app') in text.splitlines()[0], 'primary result should be application directory')

    # The compiler exposes the complete uninstaller before embedding it; copy it without running it.
    copier = root / 'copy-uninstaller.py'
    copier.write_text('import shutil, sys\nshutil.copyfile(sys.argv[1], sys.argv[2])\n', encoding='utf-8')
    uninstaller = root / 'auxiliary.exe'
    compile_source('uninstaller-source', f'''Unicode true
Name "Extract Uninstaller Fixture"
OutFile "uninstaller-source.exe"
!uninstfinalize '"{sys.executable}" "{copier}" "%1" "{uninstaller}"' = 0
Section
WriteUninstaller "$INSTDIR\\Uninstall.exe"
SectionEnd
Section "Uninstall"
DetailPrint "Fixture only; never executed"
SectionEnd
''', makensis, '.nsi')
    check(uninstaller.is_file(), 'compiler did not provide an uninstaller')

    def auxiliary_wrapper(name, child=inner, unknown_payload=False, fake=False, app_aux=False):
        auxiliary = root / 'ordinary.exe' if fake else uninstaller
        body = f'''SetOutPath "$PLUGINSDIR"
File /oname=inner.exe "{child}"
'''
        body += 'SetOutPath "$INSTDIR"\n' if app_aux else 'ReadRegStr $OUTDIR HKCU "Software\\ExtractFixture" "UnknownDir"\n'
        body += f'File /oname={"Uninstall.exe" if fake else "auxiliary.exe"} "{auxiliary}"\n'
        if unknown_payload:
            body += f'File /oname=application.exe "{root / "ordinary.exe"}"\n'
        return compile_source(name, f'''Unicode true
Name "Extract Auxiliary Fixture"
OutFile "{name}.exe"
SetCompressor /SOLID lzma
Section
{body}SectionEnd
''', makensis, '.nsi')

    optional_outer = auxiliary_wrapper('optional-uninstaller')
    output, data, text = run(optional_outer)
    check(not data['pathsResolved'] and data['requiredPathsResolved'], 'optional directory must not downgrade success')
    auxiliary = next(e for e in data['files'] if e['role'] == 'uninstaller')
    check(not auxiliary['pathResolved'], 'raw unresolved directory must remain visible')
    check((output / auxiliary['path']).read_bytes() == uninstaller.read_bytes(), 'uninstaller must be retained intact')
    check(any(p['status'] == 'preserved' for p in data['nestedPackages']), 'uninstaller must not be recursively extracted')
    child = next(p for p in data['nestedPackages'] if p['format'] == 'Inno Setup')
    check(str(output / child['output'] / 'app') in text.splitlines()[0], 'success opens the actual application directory')

    _, data, _ = run(auxiliary_wrapper('mixed-unresolved', unknown_payload=True), 299)
    check(not data['requiredPathsResolved'], 'ordinary unresolved payload must remain partial')
    _, data, _ = run(auxiliary_wrapper('fake-uninstaller-name', fake=True), 299)
    check(all(e['role'] == 'payload' for e in data['files']), 'filename alone must not classify uninstallers')
    output, data, text = run(auxiliary_wrapper('known-uninstaller-directory', app_aux=True))
    child = next(p for p in data['nestedPackages'] if p['format'] == 'Inno Setup')
    check(str(output / child['output'] / 'app') in text.splitlines()[0], 'auxiliary-only app directory must not override application output')
    _, data, _ = run(nsis('nested-optional-uninstaller', [('child.exe', optional_outer)]))
    check(data['nestedPackages'][0]['status'] == 'complete', 'auxiliary-only path status must propagate success')

    middle = nsis('middle', [('inner.exe', inner)])
    output, data, _ = run(nsis('three-levels', [('middle.exe', middle)]))
    check(data['treeFileCount'] == 3, 'three-level expansion')
    middle_out = output / data['nestedPackages'][0]['output']
    child = middle_out / report(middle_out)['nestedPackages'][0]['output']
    check((child / 'app/payload.bin').read_bytes() == payload, 'deep payload')

    _, data, _ = run(nsis('duplicate', [('first.exe', inner), ('second.exe', inner)]))
    check(data['treeFileCount'] == 3, 'duplicate should only expand once')
    check(data['nestedPackages'][0]['output'] == data['nestedPackages'][1]['output'], 'duplicate output reuse')

    unsupported = root / 'unsupported.exe'
    parsed = Inno(inner)
    raw = parsed.raw
    raw[parsed.metadata:parsed.metadata + 64] = raw[parsed.metadata:parsed.metadata + 64].replace(b'(6.', b'(9.', 1)
    unsupported.write_bytes(raw)
    _, data, _ = run(nsis('unsupported-child', [('inner.exe', unsupported)]), 299)
    check(data['nestedPackages'][0]['status'] == 'unsupported' and not data['nestedComplete'], 'unsupported status')

    parsed = Inno(inner)
    parsed.blocks[1][parsed.hash_offset] ^= 1
    corrupt = root / 'corrupt.exe'
    corrupt.write_bytes(parsed.rebuild())
    output, data, _ = run(nsis('corrupt-child', [('inner.exe', corrupt)]), 299)
    check(data['nestedPackages'][0]['status'] == 'failed', 'corrupt child status')
    check(len(list(output.rglob('_extract-report.json'))) == 1, 'failed child staging retained')
    _, data, _ = run(auxiliary_wrapper('uninstaller-with-corrupt-child', child=corrupt), 299)
    check(data['requiredPathsResolved'] and not data['nestedComplete'], 'uninstaller must not conceal an inner failure')

    for name, signature in [('zip', b'PK\x03\x04'), ('7z', b'7z\xbc\xaf\x27\x1c')]:
        archive = root / ('package.' + name)
        archive.write_bytes(signature + b'\0' * 8)  # Truncated header, not an unknown future version.
        _, data, _ = run(nsis('archive-' + name, [(archive.name, archive)]), 299)
        check(data['nestedPackages'][0]['status'] == 'failed', 'corrupt archive partial state')
        _, data, _ = run(nsis('asset-' + name, [(archive.name, archive)], app=True))
        check(not data['nestedPackages'], 'ordinary application asset flagged')

    _, data, _ = run(nsis('dynamic-child', [('inner.exe', inno('dynamic', dynamic=True))]), 299)
    check(data['nestedPackages'][0]['status'] == 'partial', 'unresolved path propagation')

    chain = inner
    for index in range(4):
        chain = nsis(f'chain-{index}', [('child.exe', chain)])
    output, data, _ = run(chain, 299)
    check(data['treeFileCount'] == 4, 'four-level depth limit')
    layer = data
    while layer['nestedPackages'][0]['output']:
        output /= layer['nestedPackages'][0]['output']
        layer = report(output)
    check(layer['nestedPackages'][0]['status'] == 'limit', 'depth limit propagation')

    many = nsis('many-files', [(f'f{i}.bin', root / 'payload.bin') for i in range(10000)], app=True)
    output, data, _ = run(nsis('large-file-count', [('child.exe', many)]), timeout=300)
    check(data['nestedPackages'][0]['status'] == 'complete' and data['treeFileCount'] == 10001, 'large nested file count')
    check(len(list(output.rglob('_extract-report.json'))) == 2, 'large child must be extracted')

    parsed = Inno(inner)
    struct.pack_into('<Q', parsed.blocks[1], parsed.size_offset, 9 * 1024**3)
    large = root / 'large.exe'
    large.write_bytes(parsed.rebuild())
    _, data, _ = run(nsis('truncated-large-child', [('child.exe', large)]), 299)
    check(data['nestedPackages'][0]['status'] == 'failed', 'truncated large child must fail')
    check('所有层' not in data['nestedPackages'][0]['message'], 'removed aggregate byte ceiling')

    children = [(f'child{i}.exe', inno(f'unique-{i}', content=f'payload-{i}'.encode())) for i in range(32)]
    _, data, _ = run(nsis('package-budget', children), 299)
    check(sum(item['status'] == 'complete' for item in data['nestedPackages']) == 31, 'package count limit')
    check(data['nestedPackages'][-1]['status'] == 'limit', '32-package limit includes root')

    (root / 'validation.json').write_text(json.dumps(runs, indent=2), encoding='utf-8')
    print(f'PASS: {len(runs)} nested extraction scenarios', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
