"""Seeded ARMv6K/Thumb-1 generator and M3 differential-test runner.

Only the standard library is imported. Toolchain/recompiler work happens in
subprocesses; generated code and logs stay in out/fuzz. No game is involved.
"""
import argparse
import json
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
CONDITIONS = ['', 'eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le']


def arm(rng, index):
    """One independently removable unit; address setup travels with memory ops."""
    d, a, b = rng.sample(range(8), 3)
    cond = rng.choice(CONDITIONS)
    family = index % 8
    if family < 2:
        op = rng.choice('and eor sub rsb add adc sbc rsc tst teq cmp cmn orr mov bic mvn'.split())
        suffix = '' if op in ('tst', 'teq', 'cmp', 'cmn') else rng.choice(['', 's'])
        form = rng.randrange(4)
        shift = rng.choice(['lsl', 'lsr', 'asr', 'ror'])
        immediate, rotate = rng.randrange(256), 2 * rng.randrange(16)
        immediate = ((immediate >> rotate) | (immediate << (32 - rotate))) & 0xffffffff
        amount = rng.randrange(32) if shift == 'lsl' else rng.randrange(1, 33 if shift != 'ror' else 32)
        operand = (f'#{immediate}' if form == 0 else f'r{b}, rrx' if form == 3 else
                   f'r{b}, {shift} ' + (f'#{amount}' if form == 1 else 'r12'))
        args = (f'r{a}, {operand}' if op in ('tst', 'teq', 'cmp', 'cmn') else
                f'r{d}, {operand}' if op in ('mov', 'mvn') else f'r{d}, r{a}, {operand}')
        return [f'{op}{suffix}{cond} {args}']
    if family == 2:
        op = rng.choice('mul mla umull smull umlal smlal clz'.split())
        if op == 'clz':
            args = f'r{d}, r{a}'
        elif op in ('mul', 'mla'):
            args = f'r{d}, r{a}, r{b}' + (', r12' if op == 'mla' else '')
        else:
            args = f'r{d}, r12, r{a}, r{b}'
        suffix = rng.choice(['', 's']) if op != 'clz' else ''
        return [f'{op}{suffix}{cond} {args}']
    if family == 3:
        media = [m + o for m in ('s', 'q', 'sh', 'u', 'uq', 'uh')
                 for o in ('add16', 'asx', 'sax', 'sub16', 'add8', 'sub8')]
        media += 'qadd qsub qdadd qdsub sel smulbb smulbt smultb smultt smulwb smulwt'.split()
        op = rng.choice(media)
        return [f'{op}{cond} r{d}, r{a}, r{b}']
    if family == 4:
        op = rng.choice('rev rev16 revsh sxtb sxth uxtb uxth sxtab sxtah uxtab uxtah ssat usat smlabb smlabt smlatb smlatt smlawb smlawt'.split())
        if op in ('ssat', 'usat'):
            args = f'r{d}, #{rng.randrange(1, 32)}, r{a}'
        elif op.startswith('smla'):
            args = f'r{d}, r{a}, r{b}, r12'
        elif op in ('sxtab', 'sxtah', 'uxtab', 'uxtah'):
            args = f'r{d}, r{a}, r{b}, ror #{rng.choice([0, 8, 16, 24])}'
        else:
            args = f'r{d}, r{a}'
        return [f'{op}{cond} {args}']
    setup = ['mov r10, #0x08000000', 'add r10, r10, #128', 'mov r11, #8']
    if family == 5:
        op = rng.choice('ldr str ldrb strb ldrh strh ldrsb ldrsh'.split())
        offset = rng.choice(['#8', '#-8', 'r11', '-r11'])
        address = rng.choice([f'[r10, {offset}]', f'[r10, {offset}]!', f'[r10], {offset}'])
        if op in ('ldr', 'str', 'ldrb', 'strb') and rng.randrange(4) == 0:
            address = '[r10, r11, lsl #1]'
        return setup + [f'{op}{cond} r{d}, {address}']
    if family == 6:
        op = rng.choice(['ldrd', 'strd', 'ldm', 'stm', 'ldrex', 'strex'])
        if op in ('ldm', 'stm'):
            return setup + [f'{op}{rng.choice(["ia", "ib", "da", "db"])}{cond} r10!, {{r0-r3}}']
        if op == 'ldrex':
            return setup + [f'ldrex{cond} r0, [r10]', f'strex{cond} r1, r2, [r10]']
        if op == 'strex':
            return setup + [f'strex{cond} r1, r2, [r10]']
        address = rng.choice(['[r10, #8]', '[r10, #-8]!', '[r10], #8', '[r10, r11]'])
        return setup + [f'{op}{cond} r0, r1, {address}']
    precision = rng.choice(['f32', 'f64'])
    reg = 's' if precision == 'f32' else 'd'
    op = rng.choice(['vadd', 'vsub', 'vmul', 'vdiv', 'vmla', 'vmls', 'vnmla', 'vnmls', 'vnmul',
                     'vsqrt', 'vabs', 'vneg', 'vcmp', 'vcmpe', 'vcvt', 'vldr', 'vstr'])
    if op in ('vldr', 'vstr'):
        return setup + [f'{op}{cond} {reg}{d}, [r10, #8]']
    if op in ('vcmp', 'vcmpe'):
        return [f'{op}{cond}.{precision} {reg}{a}, {reg}{b}', f'vmrs{cond} APSR_nzcv, fpscr']
    if op == 'vcvt':
        return [rng.choice([f'vcvt{cond}.s32.{precision} s{d}, {reg}{a}',
                            f'vcvt{cond}.u32.{precision} s{d}, {reg}{a}',
                            f'vcvt{cond}.{precision}.s32 {reg}{d}, s{a}',
                            f'vcvt{cond}.{precision}.u32 {reg}{d}, s{a}',
                            f'vcvt{cond}.f64.f32 d{d}, s{a}', f'vcvt{cond}.f32.f64 s{d}, d{a}'])]
    args = f'{reg}{d}, {reg}{a}' + ('' if op in ('vsqrt', 'vabs', 'vneg') else f', {reg}{b}')
    return [f'{op}{cond}.{precision} {args}']


def thumb(rng, index):
    d, a = rng.sample(range(6), 2)
    family = index % 5
    if family == 0:
        return [f'{rng.choice(["movs", "cmp", "adds", "subs"])} r{d}, #{rng.randrange(256)}']
    if family == 1:
        op = rng.choice('ands eors lsls lsrs asrs adcs sbcs rors tst rsbs cmp cmn orrs muls bics mvns'.split())
        args = f'r{d}, r{a}'
        if op == 'rsbs':
            args += ', #0'
        if op == 'muls':
            args += f', r{d}'
        return [f'{op} {args}']
    if family == 2:
        return [rng.choice([f'lsls r{d}, r{a}, #{rng.randrange(32)}',
                            f'lsrs r{d}, r{a}, #{rng.randrange(1, 33)}',
                            f'asrs r{d}, r{a}, #{rng.randrange(1, 33)}',
                            f'adds r{d}, r{a}, #{rng.randrange(8)}',
                            f'subs r{d}, r{a}, r5', f'mov r8, r{a}', f'add r8, r{a}',
                            f'cmp r8, r{a}', f'add r{d}, sp, #4', f'add r{d}, pc, #4',
                            f'ldr r{d}, [pc, #0]',
                            f'{rng.choice(["rev", "rev16", "revsh", "sxtb", "sxth", "uxtb", "uxth"])} r{d}, r{a}'])]
    if family == 3:
        op = rng.choice('ldr str ldrb strb ldrh strh ldrsb ldrsh'.split())
        offset = 'r7' if op in ('ldrsb', 'ldrsh') or rng.randrange(2) else '#8'
        return ['movs r6, #8', 'lsls r6, r6, #24', 'movs r7, #8', f'{op} r{d}, [r6, {offset}]']
    return rng.choice([['push {r0-r3}', 'pop {r0-r3}'],
                       ['movs r6, #8', 'lsls r6, r6, #24', 'stmia r6!, {r0-r3}'],
                       ['movs r6, #8', 'lsls r6, r6, #24', 'ldmia r6!, {r0-r3}'],
                       [f'ldr r{d}, [sp, #4]'], [f'str r{d}, [sp, #4]']])


def source(blocks, mode):
    lines = ['.syntax unified', '.fpu vfp', '.' + mode, '.text']
    for name, units in blocks.items():
        lines += ['.balign 4', '.global ' + name, '.type ' + name + ', %function']
        if mode == 'thumb':
            lines += ['.thumb_func']
        lines += [name + ':']
        if mode == 'arm':
            # M3 resets our GE flags but preserves Unicorn's between runs.
            # Establish GE=0 on both sides before conditional SEL can read it.
            lines += ['    mov r12, #0', '    uadd8 r12, r12, r12']
        for unit in units:
            lines += ['    ' + insn for insn in unit]
        lines += ['    bx lr', f'.size {name}, . - {name}']
    return '\n'.join(lines) + '\n'


class Runner:
    def __init__(self, args, directory):
        self.args, self.directory = args, directory
        self.gaps = []
        self.cc = shlex.split(os.environ.get('HOST_CC', 'clang'))
        self.armcc = shlex.split(os.environ.get('ARM_CC', 'arm-none-eabi-gcc'))
        self.objdump = shlex.split(os.environ.get('OBJDUMP', 'arm-none-eabi-objdump'))
        self.flags = shlex.split(os.environ.get('HOST_FLAGS', '-std=c11 -O1 -g -Wall -Wextra -Werror -fno-math-errno -fsanitize=address,undefined -fno-sanitize-recover=all -Iruntime'))

    def command(self, cmd, check=True):
        result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=120)
        if check and result.returncode:
            raise RuntimeError(f'{shlex.join(cmd)}\n{result.stdout}')
        return result

    def build(self, blocks, mode):
        base = self.directory / mode
        assembly, elf, generated = base.with_suffix('.S'), base.with_suffix('.elf'), base.with_suffix('.c')
        while True:
            assembly.write_text(source(blocks, mode))
            self.command(self.armcc + ['-mcpu=mpcore', '-mfpu=vfp', '-mfloat-abi=hard', '-nostdlib',
                                      '-Wl,-Ttext=0x00100000', '-Wl,-e,' + next(iter(blocks)),
                                      str(assembly), '-o', str(elf)])
            disassembly = self.command(self.objdump + ['-d', str(elf)]).stdout
            base.with_suffix('.dis').write_text(disassembly)
            self.instructions = {}
            current, index = None, 0
            for line in disassembly.splitlines():
                label = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
                if label:
                    current, index = label[1], 0
                insn = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]{4,8})\s+(.+)', line)
                if insn and current in blocks:
                    self.instructions[int(insn[1], 16)] = (current, index - (2 if mode == 'arm' else 0), insn[2], insn[3])
                    index += 1
            translated = self.command([sys.executable, '-m', 'recomp', '--trace', str(elf), '-o', str(generated)], False)
            base.with_suffix('.translate.log').write_text(translated.stdout)
            if translated.returncode == 0:
                break
            error = re.search(r'recomp: error: ([0-9a-f]{8}):', translated.stdout)
            if not error or int(error[1], 16) not in self.instructions:
                raise RuntimeError(translated.stdout)
            name, index, encoding, text = self.instructions[int(error[1], 16)]
            offset = 0
            for unit_index, unit in enumerate(blocks[name]):
                if offset <= index < offset + len(unit):
                    self.gaps.append({'mode': mode, 'instruction': text, 'encoding': encoding,
                                      'reason': translated.stdout.strip()})
                    (self.directory / 'unsupported.json').write_text(json.dumps(self.gaps, indent=2) + '\n')
                    del blocks[name][unit_index]
                    break
                offset += len(unit)
            else:
                raise RuntimeError('Rejected function epilogue: ' + translated.stdout)
        exe = base.with_suffix('.diff')
        self.command(self.cc + self.flags + ['-DRECOMP_TRACING=1', 'tests/m3/diff.c', str(generated),
                                          'runtime/runtime.c', '-lunicorn', '-lm', '-pthread', '-o', str(exe)])
        return exe

    def run(self, exe, name):
        return self.command([str(exe), str(self.args.runs), '+' + name], False)

    def shrink(self, name, units, mode, signature):
        # Delete chunks, then individual units, preserving the failure mnemonic.
        # Address setup and balanced stack operations are indivisible units.
        units = list(units)
        width = max(1, len(units) // 2)
        while width:
            changed = False
            for start in range(0, len(units), width):
                trial = units[:start] + units[start + width:]
                result = self.run(self.build({name: list(trial)}, mode), name)
                if result.returncode == 1 and signature in result.stdout:
                    units, changed = trial, True
                    break
            if not changed:
                width //= 2
        return units


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('-n', type=int, default=200, help='functions per mode')
    parser.add_argument('--runs', type=int, default=10)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('out/fuzz'))
    args = parser.parse_args()
    if args.n < 1 or args.runs < 1:
        parser.error('N and runs must be positive')
    directory = (ROOT / args.output / str(args.seed)).resolve()
    if not directory.is_relative_to(ROOT / 'out'):
        parser.error('generated output must stay under out/')
    directory.mkdir(parents=True, exist_ok=True)
    rng, started = random.Random(args.seed), time.monotonic()
    runner = Runner(args, directory)
    summary = {'seed': args.seed, 'runs': args.runs, 'modes': {}, 'divergences': []}
    for mode, generator in [('arm', arm), ('thumb', thumb)]:
        blocks = {}
        for i in range(args.n):
            units = []
            # Count actual instructions, including safe address setup.
            wanted = rng.randint(10, 40)
            while len([x for unit in units for x in unit]) < wanted:
                unit = generator(rng, len(units) + i)
                if sum(map(len, units)) + len(unit) > 40:
                    break
                units.append(unit)
            blocks[f'fuzz_{mode}_{i:04d}'] = units
        (directory / f'{mode}-original.S').write_text(source(blocks, mode))
        if args.generate_only:
            continue
        exe = runner.build(blocks, mode)
        summary['modes'][mode] = {'functions': len(blocks), 'instructions': sum(sum(map(len, u)) + (3 if mode == 'arm' else 1) for u in blocks.values())}
        # Run functions separately: RNG starts identically during minimization.
        failures = []
        for position, (name, units) in enumerate(blocks.items(), 1):
            result = runner.run(exe, name)
            (directory / f'{name}.log').write_text(result.stdout)
            if result.returncode:
                match = re.search(r'after [0-9a-f]+ `([^ `]+)', result.stdout)
                if result.returncode != 1 or not match:
                    raise RuntimeError(f'{name}: harness failed without a usable divergence\n{result.stdout}')
                failures.append((name, units, match[1], dict(runner.instructions)))
            if position % 25 == 0:
                print(f'{mode}: checked {position}/{len(blocks)} functions', flush=True)
        for name, units, mnemonic, instructions in failures:
            minimal = runner.shrink(name, units, mode, '`' + mnemonic + ' ')
            found = ROOT / 'tests/fuzz/found'
            found.mkdir(parents=True, exist_ok=True)
            path = found / f'{mnemonic}-{args.seed}-{name}.S'
            encodings = '\n'.join(f'@ {encoding}: {text}' for fn, _, encoding, text in instructions.values() if fn == name)
            path.write_text(f'@ Seed {args.seed}; mode {mode}; M3 runs {args.runs}.\n'
                            '@ Deletion-minimal instruction units; replay with the M3 driver.\n'
                            '@ Original encodings before shrinking:\n' + encodings + '\n' + source({name: minimal}, mode))
            final = runner.run(runner.build({name: minimal}, mode), name)
            (directory / (path.stem + '-minimized.log')).write_text(final.stdout)
            summary['divergences'].append({'function': name, 'mnemonic': mnemonic, 'reproducer': str(path.relative_to(ROOT)),
                                          'units': len(minimal), 'assessment': 'Unclassified: compare ARM semantics before assigning blame.'})
            print('DIVERGENCE:', path.relative_to(ROOT), flush=True)
        print(mode, summary['modes'][mode], flush=True)
    summary['unsupported'] = runner.gaps
    summary['seconds'] = round(time.monotonic() - started, 2)
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    (directory / 'unsupported.json').write_text(json.dumps(runner.gaps, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return bool(summary['divergences'])


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        sys.exit(2)
