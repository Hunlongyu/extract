"""Generate inert split packages with official ISCC/MakeCab; never run installers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import uuid

def check(value, message):
    if not value: raise AssertionError(message)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', required=True, type=Path)
    p.add_argument('--work-root', required=True, type=Path)
    p.add_argument('--compiler', type=Path)
    p.add_argument('--only', choices=('all', 'inno', 'cab'), default='all')
    args = p.parse_args()
    compiler = args.compiler or Path(os.environ['ProgramFiles(x86)']) / 'Inno Setup 6/ISCC.exe'
    if not compiler.is_file(): print('SKIP: official ISCC required'); return 77
    exe = args.executable.resolve(); root = args.work_root.resolve() / uuid.uuid4().hex; root.mkdir(parents=True)
    cache = root / 'cache'; cache.mkdir(); env = dict(os.environ, TEMP=str(cache), TMP=str(cache))
    compiler_version = None
    cases = []; known = {'before.txt': b'first file\n', 'large.dat': random.Random(301).randbytes(9300013),
                         'after.dat': random.Random(523).randbytes(150019), 'empty.txt': b''}
    source = root / 'source'; source.mkdir()
    for name, data in known.items(): (source / name).write_bytes(data)
    def command(args, cwd=None):
        r = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, timeout=120)
        check(r.returncode == 0, f'fixture compiler failed: {r.stdout!r} {r.stderr!r}')
        return r
    def run(package, expected=0, message=None):
        inputs = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in package.parent.iterdir() if f.is_file()}
        output = root / ('output-' + uuid.uuid4().hex); output.mkdir()
        r = subprocess.run([exe, '--quiet', '--output', output, package], capture_output=True, env=env, timeout=120)
        check(r.returncode == expected, f'{package}: expected {expected}, got {r.returncode}: {r.stdout!r} {r.stderr!r}')
        if message: check(message in (r.stdout + r.stderr).decode('utf-8'), f'{package}: missing {message}')
        check(not list(cache.iterdir()), 'cache leaked')
        check(inputs == {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in package.parent.iterdir() if f.is_file()}, 'input changed')
        record = {'input': package.relative_to(root).as_posix(), 'expectedExitCode': expected, 'inputs': inputs}
        cases.append(record)
        if expected:
            check(not list(output.iterdir()), 'failed package committed output'); return
        targets = list(output.iterdir()); check(len(targets) == 1, 'output directory count')
        report = json.loads((targets[0] / '_extract-report.json').read_text(encoding='utf-8'))
        check(report['status'] == 'complete' and len(report['files']) == len(known), 'file catalog incomplete')
        for f in report['files']:
            name = Path(f['originalPath'].replace('\\', '/')).name
            check((targets[0] / f['path']).read_bytes() == known[name], f'payload mismatch {name}')
            check(f['sha256'] == hashlib.sha256(known[name]).hexdigest(), 'output digest')
            if report['format'] == 'Inno Setup': check(f['sourceHashVerified'], 'Inno source hash missing')
            if report['format'] == 'MSI': check(f['msiHashVerified'], 'MSI file hash missing')
        record.update(format=report['format'], files=len(report['files']))
    def variant(directory, name):
        target = root / name; shutil.copytree(directory, target); return target

    for method in (('none', 'zip', 'bzip', 'lzma', 'lzma2') if args.only != 'cab' else ()):
        for solid in (False, True):
            dest = root / f'inno-{method}-{solid}'; dest.mkdir()
            settings = f'''[Setup]
AppName=Inert split fixture
AppVersion=1
DefaultDirName={{autopf}}\\Fixture
OutputDir={dest}
OutputBaseFilename=split
Compression={method}
SolidCompression={'yes' if solid else 'no'}
LZMAUseSeparateProcess=no
DiskSpanning=yes
DiskSliceSize=4194304
SlicesPerDisk={3 if solid else 1}
ReserveBytes=0
[Files]
'''
            for name in known: settings += f'Source: "{source / name}"; DestDir: "{{app}}"\n'
            script = root / 'fixture.iss'; script.write_text(settings, encoding='utf-8-sig')
            built = command([compiler, script] if compiler_version is None else [compiler, '/Q', script])
            if compiler_version is None:
                match = re.search(rb'Compiler engine version: Inno Setup (\d+\.\d+\.\d+)', built.stdout + built.stderr)
                check(match, 'ISCC version missing'); compiler_version = match[1].decode('ascii')
            package = dest / 'split.exe'
            bins = sorted(dest.glob('*.bin')); check(len(bins) >= 3, 'fixture did not span')
            run(package)
            if method == 'lzma2' and solid:
                bad = variant(dest, 'inno-missing'); (bad / bins[1].name).unlink(); run(bad / package.name, 30, bins[1].name)
                bad = variant(dest, 'inno-truncated'); f = bad / bins[-1].name; f.write_bytes(f.read_bytes()[:-1]); run(bad / package.name, 13, '长度')
                bad = variant(dest, 'inno-bad-id'); f = bad / bins[0].name; b = bytearray(f.read_bytes()); b[0] ^= 1; f.write_bytes(b); run(bad / package.name, 13, '标识')
                bad = variant(dest, 'inno-swapped'); a,b = [bad / f.name for f in bins[:2]]; aa,bb=a.read_bytes(),b.read_bytes(); a.write_bytes(bb); b.write_bytes(aa); run(bad / package.name, 13)
                bad = variant(dest, 'inno-damaged'); f = bad / bins[1].name; b = bytearray(f.read_bytes()); b[500] ^= 128; f.write_bytes(b); run(bad / package.name, 13)

    known['large.dat'] = random.Random(301).randbytes(720013)
    (source / 'large.dat').write_bytes(known['large.dat'])
    for method in (('MSZIP', 'LZX', 'NONE') if args.only != 'inno' else ()):
        dest = root / ('cab-' + method); dest.mkdir()
        directives = ['.Set Cabinet=ON', '.Set Compress=' + ('OFF' if method == 'NONE' else 'ON'),
                      '.Set CompressionType=' + ('MSZIP' if method == 'NONE' else method),
                      '.Set CabinetNameTemplate=volume*.cab', '.Set MaxCabinetSize=131072', '.Set MaxDiskSize=131072',
                      f'.Set DiskDirectoryTemplate="{dest}"', f'.Set RptFileName="{root / "cab.rpt"}"', f'.Set InfFileName="{root / "cab.inf"}"']
        directives += [f'"{source / name}" {name}' for name in known]
        ddf = root / 'fixture.ddf'; ddf.write_text('\n'.join(directives), encoding='mbcs')
        command([Path(os.environ['SystemRoot']) / 'System32/makecab.exe', '/F', ddf])
        cabs = sorted(dest.glob('*.cab'), key=lambda f: int(f.stem[6:])); check(len(cabs) >= 3, 'CAB did not span')
        for cab in (cabs[0], cabs[len(cabs)//2], cabs[-1]): run(cab)
        if method == 'MSZIP':
            bad = variant(dest, 'cab-case-name'); original = bad / cabs[0].name
            renamed = bad / cabs[0].name.upper(); original.rename(renamed); run(renamed)
            bad = variant(dest, 'cab-traversal'); f=bad/cabs[0].name; f.write_bytes(f.read_bytes().replace(b'volume2.cab', b'../evil.cab', 1)); run(f,5)
            bad = variant(dest, 'cab-cycle'); f=bad/cabs[1].name; f.write_bytes(f.read_bytes().replace(b'volume3.cab', b'volume1.cab', 1)); run(bad/cabs[0].name,13)
            bad = variant(dest, 'cab-continued-name'); f=bad/cabs[1].name; b=bytearray(f.read_bytes()); at=struct.unpack_from('<I',b,16)[0]; b[at+16] ^= 1; f.write_bytes(b); run(bad/cabs[0].name,13,'跨卷文件')
            # The media boundary includes every file whose first CFFILE entry
            # appears in that cabinet. Continued rows are not counted twice.
            last = 0; cabinet_sequences = []
            for cab in cabs:
                b = cab.read_bytes(); at = struct.unpack_from('<I', b, 16)[0]
                for _ in range(struct.unpack_from('<H', b, 28)[0]):
                    folder = struct.unpack_from('<H', b, at + 8)[0]
                    if folder not in (0xfffd, 0xffff): last += 1
                    at = b.index(0, at + 16) + 1
                cabinet_sequences.append(last)
            for mode in ('external', 'embedded', 'mixed'):
                msi_dir = root / ('msi-' + mode); msi_dir.mkdir()
                spec = {'output': str(msi_dir / 'split.msi'),
                        'files': [{'name': n, 'source': str(source/n)} for n in known], 'cabs': []}
                for i,cab in enumerate(cabs):
                    embedded = mode == 'embedded' or (mode == 'mixed' and i % 2 == 0)
                    spec['cabs'].append({'name': cab.name, 'source': str(cab), 'embedded': embedded, 'lastSequence': cabinet_sequences[i]})
                    if not embedded: shutil.copyfile(cab, msi_dir / cab.name)
                spec_path = root / 'msi-spec.json'; spec_path.write_text(json.dumps(spec), encoding='utf-8')
                command(['pwsh', '-NoProfile', '-File', Path(__file__).with_name('make_spanned_msi.ps1'), '-Spec', spec_path])
                run(msi_dir / 'split.msi')
                if mode == 'external':
                    bad = variant(msi_dir, 'msi-missing'); (bad / cabs[1].name).unlink(); run(bad / 'split.msi', 30, cabs[1].name)
                if mode == 'embedded':
                    # A chain name outside Media must never cause filesystem fallback.
                    spec['output'] = str(msi_dir / 'undeclared.msi'); spec['cabs'].pop(1)
                    spec_path.write_text(json.dumps(spec), encoding='utf-8')
                    command(['pwsh', '-NoProfile', '-File', Path(__file__).with_name('make_spanned_msi.ps1'), '-Spec', spec_path])
                    run(msi_dir / 'undeclared.msi', 13, 'Media 未声明')
            bad = variant(dest, 'cab-missing'); (bad / cabs[1].name).unlink(); run(bad / cabs[0].name, 30, cabs[1].name)
            bad = variant(dest, 'cab-wrong-set'); f=bad/cabs[1].name; b=bytearray(f.read_bytes()); b[32] ^= 1; f.write_bytes(b); run(bad/cabs[0].name,13,'集合')
            bad = variant(dest, 'cab-wrong-index'); f=bad/cabs[1].name; b=bytearray(f.read_bytes()); b[34] ^= 1; f.write_bytes(b); run(bad/cabs[0].name,13,'编号')
            bad = variant(dest, 'cab-truncated'); f=bad/cabs[1].name; f.write_bytes(f.read_bytes()[:-1]); run(bad/cabs[0].name,13,'长度')
            bad = variant(dest, 'cab-damaged'); f=bad/cabs[1].name; b=bytearray(f.read_bytes()); b[-200] ^= 64; f.write_bytes(b); run(bad/cabs[0].name,13)
    (root / 'results.json').write_text(json.dumps({'compiler': str(compiler), 'compilerVersion': compiler_version, 'cases': cases}, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'PASS: {len(cases)} split-package cases; {root}', flush=True)
    return 0

if __name__ == '__main__': raise SystemExit(main())
