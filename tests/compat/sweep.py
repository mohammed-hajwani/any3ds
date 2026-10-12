"""Compatibility sweep: build, recompile and run every example listed in
tests/compat/examples.txt on the HLE, classify the outcome, and write
docs/compat.md. Run through `make compat` (inside WSL).

    python tests/compat/sweep.py [--jobs N] [--only SUBSTRING]

For each example: copy it from /opt/devkitpro/examples/3ds into
out/compat/<path>, build it with its own Makefile, recompile the .elf,
link the C with runtime/ + hle/ + tests/m6/runner.c, and run it.

Exit status 1 if any example falls behind its expected outcome.
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = Path('/opt/devkitpro/examples/3ds')
OUT = ROOT / 'out' / 'compat'
LIST = ROOT / 'tests' / 'compat' / 'examples.txt'
HASHES = ROOT / 'tests' / 'compat' / 'hashes.txt'
REPORT = ROOT / 'docs' / 'compat.md'

# START at frame 60; then, for apps whose exit prompt wants B, B at frame 80.
DEFAULT_OPTIONS = ['--hold', '60:8', '--hold', '70:0', '--hold', '80:2']
# Outcomes, worst to best; an example must reach at least its expected one.
RANK = ['build-failed', 'recomp-failed', 'c-failed', 'stops', 'runs']


@dataclass
class Example:
    path: str
    category: str
    expected: str
    options: list


@dataclass
class Result:
    example: Example
    status: str = ''
    detail: str = ''
    notes: list = field(default_factory=list)
    frame_hash: str = ''


def load_examples():
    examples = []
    for line in LIST.read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        path, category, expected, *options = shlex.split(line)
        if expected != '?' and expected not in RANK:
            sys.exit(f'{LIST}: {path}: unknown expected outcome {expected!r}')
        examples.append(Example(path, category, expected, options or DEFAULT_OPTIONS))
    return examples


def load_hashes():
    hashes = {}
    if HASHES.exists():
        for line in HASHES.read_text().splitlines():
            if line.strip() and not line.startswith('#'):
                digest, path = line.split()
                hashes[path] = digest
    return hashes


def run(cmd, cwd, log, timeout):
    """Run cmd, write its output to log; return the exit code (124 on timeout)."""
    with open(log, 'w') as f:
        try:
            return subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            f.write(f'\n(timed out after {timeout} s)\n')
            return 124


def first_line(path, pattern):
    for line in Path(path).read_text(errors='replace').splitlines():
        if re.search(pattern, line):
            return line.strip().replace(f'{ROOT}/', '')     # repo-relative paths in the report
    return ''


def classify_run(code, log):
    """Turn the runner's exit code and log into (status, detail)."""
    if 'exited via svcExitProcess' in log:
        return 'runs', ''
    if 'every thread has finished' in log:
        return 'runs', 'all threads ended without svcExitProcess'
    patterns = [
        (r'hle: svc (0x[0-9a-f]+) \((\S+)\) is not implemented', lambda m: f'svc {m[1]} {m[2]}'),
        (r'hle: (\S+) command (0x[0-9a-f]+) \(header', lambda m: f'IPC {m[1]} {m[2]}'),
        (r'^gpu: (.*)', lambda m: f'GPU: {m[1]}'),
        (r'hle: deadlock', lambda m: 'deadlock'),
        (r'hle: stopping: still running after', lambda m: 'virtual-time limit (hang?)'),
        (r'hle: guest called svcBreak', lambda m: 'svcBreak'),
        (r'recomp: (.*?)(?: \(lr = .*)?$', lambda m: f'recomp: {m[1]}'),
        (r'ERROR: AddressSanitizer: (\S+)', lambda m: f'ASan: {m[1]}'),
        (r'runtime error: (.*)', lambda m: f'UBSan: {m[1]}'),
    ]
    # The specific patterns first, anywhere in the log; then the last plain
    # "hle:" line that isn't a warning the run survived.
    lines = log.splitlines()
    for pattern, describe in patterns:
        for line in lines:
            m = re.search(pattern, line)
            if m:
                return 'stops', describe(m)
    survivable = ('GetServiceHandle', 'FS OpenArchive')
    for line in reversed(lines):
        if line.startswith('hle: ') and not any(s in line for s in survivable):
            return 'stops', line
    if code == 124:
        return 'stops', 'real-time timeout'
    return 'stops', f'exit {code}'


def sweep_one(ex, args):
    r = Result(ex)
    work = OUT / ex.path
    if not args.keep_build or not list(work.glob('*.elf')):
        shutil.rmtree(work, ignore_errors=True)
        work.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(EXAMPLES / ex.path, work)
        if run(['make', '-j4'], work, work / 'build.log', 600) != 0:
            line = first_line(work / 'build.log', r'fatal error|error:|Error|No package') or 'see build.log'
            r.status, r.detail = 'build-failed', line
            return r
    elfs = sorted(work.glob('*.elf'))
    if not elfs:
        r.status, r.detail = 'build-failed', first_line(work / 'build.log', r'not found!') or 'no .elf (a library?)'
        return r
    env_py = os.environ.get('PYTHON', str(ROOT / '.venv' / 'bin' / 'python'))
    if run([env_py, '-m', 'recomp', str(elfs[0]), '-o', str(work / 'gen.c')], ROOT, work / 'recomp.log', 600) != 0:
        r.status, r.detail = 'recomp-failed', first_line(work / 'recomp.log', r'error') or 'see recomp.log'
        return r
    cc = shlex.split(os.environ.get('HOST_CC', 'clang'))
    flags = shlex.split(os.environ.get('HOST_FLAGS', '-std=c11 -O1 -g -Wall -Wextra -Werror -fno-math-errno '
                                       '-fsanitize=undefined,address -fno-sanitize-recover=all -Iruntime'))
    os_layer = sorted(str(p) for d in ('hle', 'gpu') for p in (ROOT / d).glob('*.c'))
    cmd = cc + flags + ['-Ihle', '-Igpu', '-pthread', 'tests/m6/runner.c', str(work / 'gen.c'), 'runtime/runtime.c',
                        *os_layer, '-lm', '-o', str(work / 'run')]
    if run(cmd, ROOT, work / 'cc.log', 900) != 0:
        r.status, r.detail = 'c-failed', first_line(work / 'cc.log', r'error') or 'see cc.log'
        return r
    frame = work / 'frame.bmp'
    frame.unlink(missing_ok=True)
    options = list(ex.options)
    built = elfs[0].with_suffix('.3dsx')
    if (work / 'romfs').is_dir() and built.exists() and '--romfs' not in options:
        options += ['--romfs', built.name]      # the RomFS the .3dsx carries
    code = run(['./run', *options, '--dump', 'frame.bmp'], work, work / 'run.log', 180)
    log = (work / 'run.log').read_text(errors='replace')
    r.status, r.detail = classify_run(code, log)
    r.notes = sorted(set(re.findall(r'GetServiceHandle\("([^"]+)"\)', log)))
    if frame.exists():
        r.frame_hash = subprocess.run(['sha256sum', str(frame)], capture_output=True, text=True).stdout.split()[0]
    return r


def write_report(results, hashes):
    by_status = {s: [r for r in results if r.status == s] for s in reversed(RANK)}
    lines = [
        '# Compatibility: devkitPro 3ds-examples',
        '',
        'Generated by `make compat` (tests/compat/sweep.py): each public-domain example from devkitPro\'s',
        '3ds-examples is built, recompiled to C and run on our HLE (`hle/`). `runs` means it exited',
        'cleanly via svcExitProcess after START was pressed; `stops` names the first missing piece.',
        '',
        '| Outcome | Examples |',
        '|---|---|',
    ]
    for status, rs in by_status.items():
        lines.append(f'| {status} | {len(rs)} |')
    lines += [
        '',
        'What the remaining blockers mean:',
        '',
        '- **GSP GX commands need the GPU**: citro2d/citro3d programs. Their first GX command is a',
        '  MemoryFill (clearing a render target); right after it comes ProcessCommandList, which needs',
        '  an emulated PICA200 GPU. That is the next big piece (and where reusing Azahar would pay off).',
        '- **APT GetSharedFont**: the system font is part of the console\'s firmware. We can\'t ship it,',
        '  and the project never handles firmware.',
        '- **build-failed**: examples that need portlibs (box2d, opus, vorbis, modplug) or ImageMagick,',
        '  which are not installed; `mvd`, which wants a video file supplied; a library template that',
        '  builds no program; and `qtm/eye_position`, which no longer compiles against the current libctru.',
        '- `runs` means a clean exit, not a perfect run: an example whose service is missing (network,',
        '  camera, audio) prints its error and exits when asked. The frame hashes pin down what it showed.',
    ]
    blockers = {}
    for r in results:
        if r.status != 'runs':
            blockers.setdefault(f'{r.status}: {r.detail}', []).append(r.example.path)
    lines += ['', '## Blockers, most common first', '', '| Blocker | Count | Examples |', '|---|---|---|']
    for blocker, paths in sorted(blockers.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        lines.append(f'| {md(blocker)} | {len(paths)} | {", ".join(paths)} |')
    lines += ['', '## Every example', '', '| Example | Needs | Outcome | First blocker | Services asked for but missing | Frame |',
              '|---|---|---|---|---|---|']
    for r in results:
        p = r.example.path
        frame = ''
        if r.frame_hash:
            frame = 'matches' if hashes.get(p) == r.frame_hash else ('CHANGED' if p in hashes else 'new')
        lines.append(f'| {p} | {r.example.category} | {r.status} | {md(r.detail)} | {", ".join(r.notes)} | {frame} |')
    REPORT.parent.mkdir(exist_ok=True)
    REPORT.write_text('\n'.join(lines) + '\n')


def md(text):
    return text.replace('|', '\\|')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--only', action='append', help='only examples whose path contains this (repeatable)')
    ap.add_argument('--record', action='store_true',
                    help='after looking at the frames: make the outcomes and frame hashes of this run the expected ones')
    ap.add_argument('--keep-build', action='store_true', help='reuse already-built example .elf files')
    args = ap.parse_args()

    examples = load_examples()
    if args.only:
        examples = [e for e in examples if any(o in e.path for o in args.only)]
    hashes = load_hashes()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(sweep_one, ex, args): ex for ex in examples}
        for f in concurrent.futures.as_completed(futures):
            r = f.result()
            print(f'{r.status:13} {r.example.path:40} {r.detail}', flush=True)
    results = [f.result() for f in futures]
    results.sort(key=lambda r: r.example.path)
    if not args.only:
        write_report(results, hashes)
        print(f'wrote {REPORT.relative_to(ROOT)}')

    failed = []
    for r in results:
        ex = r.example
        if ex.expected != '?' and RANK.index(r.status) < RANK.index(ex.expected):
            failed.append(f'{ex.path}: expected {ex.expected}, got {r.status} ({r.detail})')
        if r.frame_hash and ex.path in hashes and hashes[ex.path] != r.frame_hash:
            failed.append(f'{ex.path}: the frame on screen changed (tests/compat/hashes.txt)')
    counts = {s: sum(r.status == s for r in results) for s in RANK}
    print('=== compat: ' + ', '.join(f'{n} {s}' for s, n in reversed(counts.items())) + ' ===')
    for f in failed:
        print('REGRESSION ' + f)
    if args.record:
        record(results, hashes)
        return 0
    return 1 if failed else 0


def record(results, hashes):
    """Make this run's outcomes the expected ones (examples.txt) and its
    frames the reference frames (hashes.txt)."""
    status = {r.example.path: r.status for r in results}
    out = []
    for line in LIST.read_text().splitlines():
        fields = line.split()
        if fields and not line.startswith('#') and fields[0] in status:
            # keep the column layout: replace the third field in place
            path, category = fields[0], fields[1]
            options = line.split(None, 3)[3] if len(fields) > 3 else ''
            line = f'{path:36} {category:12} {status[path]:13}{" " + options if options else ""}'.rstrip()
        out.append(line)
    LIST.write_text('\n'.join(out) + '\n')
    for r in results:
        if r.status == 'runs' and r.frame_hash:
            hashes[r.example.path] = r.frame_hash
    HASHES.write_text('# sha256 of the last frame (both screens) each example showed: tests/compat/sweep.py --record\n'
                      + ''.join(f'{digest}  {path}\n' for path, digest in sorted(hashes.items())))
    print(f'recorded outcomes in {LIST.relative_to(ROOT)} and frames in {HASHES.relative_to(ROOT)}')


if __name__ == '__main__':
    sys.exit(main())
