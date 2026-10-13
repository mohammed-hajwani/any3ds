"""Draw docs/progress.svg: how much of the 3DS we cover, as treemaps.

    python tools/progress.py                     (make progress)
    python tools/progress.py --refresh-services  re-read 3dbrew's service pages

Three maps, one small cell per item, green when implemented:
  - OS services: every command 3dbrew documents for each service (its
    command list is cached in tools/progress_services.json, made from the
    3dbrew pages in out/ref/3dbrew by --refresh-services). A command counts
    as implemented when a `case` for it is in our handler, or it is in a
    stub table (hle/stubs.c). Services we don't have at all show all grey.
  - CPU instructions: every ARMv6K instruction (ARM and Thumb share them)
    plus VFPv2; implemented = the translator has a handler for it.
  - GPU shader instructions: the PICA200 shader opcodes (3dbrew:
    GPU/Shader_Instruction_Set); implemented = gpu/shader.c handles it.
Plus badges, and the compatibility count from docs/compat.md.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
SERVICES_JSON = ROOT / 'tools' / 'progress_services.json'
OUT = ROOT / 'docs' / 'progress.svg'

# ---------------------------------------------------------------- data

# 3dbrew page -> (short name, how we implement it: [(file, handler or stub table)])
SERVICE_PAGES = {
    'NS_and_APT_Services': ('APT', [('hle/apt.c', 'apt_handle')]),
    'GSP_Services': ('GSP', [('hle/gsp.c', 'gsp_handle')]),
    'HID_Services': ('HID', [('hle/hid.c', 'hid_handle')]),
    'Filesystem_services': ('FS', [('hle/fs.c', 'fs_handle')]),
    'Config_Services': ('CFG', [('hle/cfg.c', 'cfg_handle')]),
    'DSP_Services': ('DSP', [('hle/dsp.c', 'dsp_handle')]),
    'RO_Services': ('RO', [('hle/ro.c', 'ro_handle')]),
    'NDM_Services': ('NDM', [('hle/stubs.c', 'ndm_cmds')]),
    'PTM_Services': ('PTM', [('hle/stubs.c', 'ptm_cmds')]),
    'Friend_Services': ('FRD', []),
    'BOSS_Services': ('BOSS', []),
    'ACT_Services': ('ACT', []),
    'AC_Services': ('AC', []),
    'CECD_Services': ('CECD', []),
    'MIC_Services': ('MIC', []),
    'IR_Services': ('IR', []),
    'CSND_Services': ('CSND', []),
    'Camera_Services': ('CAM', []),
    'Socket_Services': ('SOC', []),
    'HTTP_Services': ('HTTP', []),
    'SSL_Services': ('SSL', []),
    'NWM_Services': ('NWM', []),
    'QTM_Services': ('QTM', []),
    'Application_Manager_Services': ('AM', []),
    'NIM_Services': ('NIM', []),
}
# Pages that document other services too: where the one we mean starts and ends.
SECTION = {'NS_and_APT_Services': ('=APT Services=', None),
           'Filesystem_services': ('fs:USER', '== Filesystem service "fs:LDR"')}
# srv: (the service manager) is documented in prose; its commands, from 3dbrew "Services API".
SRV_COMMANDS = {0x1: 'RegisterClient', 0x2: 'EnableNotification', 0x3: 'RegisterService',
                0x4: 'UnregisterService', 0x5: 'GetServiceHandle', 0x6: 'RegisterPort',
                0x7: 'UnregisterPort', 0x8: 'GetPort', 0x9: 'Subscribe', 0xA: 'Unsubscribe',
                0xB: 'ReceiveNotification', 0xC: 'PublishToSubscriber', 0xD: 'PublishAndGetSubscriber',
                0xE: 'IsServiceRegistered'}

ARMV6K = '''
AND EOR SUB RSB ADD ADC SBC RSC TST TEQ CMP CMN ORR MOV BIC MVN LSL LSR ASR ROR RRX
MUL MLA UMULL UMLAL SMULL SMLAL UMAAL
SMLABB SMLABT SMLATB SMLATT SMLAWB SMLAWT SMULBB SMULBT SMULTB SMULTT SMULWB SMULWT
SMLALBB SMLALBT SMLALTB SMLALTT
SMUAD SMUADX SMUSD SMUSDX SMLAD SMLADX SMLSD SMLSDX SMLALD SMLALDX SMLSLD SMLSLDX
SMMUL SMMULR SMMLA SMMLAR SMMLS SMMLSR
QADD QSUB QDADD QDSUB SSAT USAT SSAT16 USAT16
SADD16 SADD8 SSUB16 SSUB8 SASX SSAX QADD16 QADD8 QSUB16 QSUB8 QASX QSAX
SHADD16 SHADD8 SHSUB16 SHSUB8 SHASX SHSAX UADD16 UADD8 USUB16 USUB8 UASX USAX
UQADD16 UQADD8 UQSUB16 UQSUB8 UQASX UQSAX UHADD16 UHADD8 UHSUB16 UHSUB8 UHASX UHSAX
SXTB SXTH SXTB16 UXTB UXTH UXTB16 SXTAB SXTAH SXTAB16 UXTAB UXTAH UXTAB16
REV REV16 REVSH SEL PKHBT PKHTB CLZ USAD8 USADA8
LDR LDRB LDRH LDRSB LDRSH LDRD LDRT LDRBT STR STRB STRH STRD STRT STRBT
LDM LDMDA LDMDB LDMIB STM STMDA STMDB STMIB PUSH POP SWP SWPB
LDREX LDREXB LDREXH LDREXD STREX STREXB STREXH STREXD CLREX
B BL BX BLX BXJ SVC BKPT CPS SETEND SRSDA SRSDB SRSIA SRSIB RFEDA RFEDB RFEIA RFEIB MRS MSR
CDP CDP2 MCR MCR2 MRC MRC2 MCRR MRRC LDC LDC2 STC STC2
NOP YIELD WFE WFI SEV PLD UDF
VABS VADD VCMP VCMPE VCVT VCVTR VDIV VLDMIA VLDMDB VLDR VMLA VMLS VMOV VMRS VMSR VMUL VNEG
VNMLA VNMLS VNMUL VPOP VPUSH VSQRT VSTMIA VSTMDB VSTR VSUB FLDMIAX FLDMDBX FSTMIAX FSTMDBX
'''.split()
HINTS_ONLY_NOP = {'YIELD', 'WFE', 'WFI', 'SEV'}      # Capstone's HINT; only nop is translated

PICA_OPCODES = {
    0x00: 'ADD', 0x01: 'DP3', 0x02: 'DP4', 0x03: 'DPH', 0x04: 'DST', 0x05: 'EX2', 0x06: 'LG2',
    0x07: 'LITP', 0x08: 'MUL', 0x09: 'SGE', 0x0A: 'SLT', 0x0B: 'FLR', 0x0C: 'MAX', 0x0D: 'MIN',
    0x0E: 'RCP', 0x0F: 'RSQ', 0x12: 'MOVA', 0x13: 'MOV', 0x18: 'DPHI', 0x19: 'DSTI', 0x1A: 'SGEI',
    0x1B: 'SLTI', 0x20: 'BREAK', 0x21: 'NOP', 0x22: 'END', 0x23: 'BREAKC', 0x24: 'CALL',
    0x25: 'CALLC', 0x26: 'CALLU', 0x27: 'IFU', 0x28: 'IFC', 0x29: 'LOOP', 0x2A: 'EMIT',
    0x2B: 'SETEMIT', 0x2C: 'JMPC', 0x2D: 'JMPU', 0x2E: 'CMP', 0x30: 'MADI', 0x38: 'MAD',
}


def refresh_services():
    """Command lists from the cached 3dbrew pages (out/ref/3dbrew/svc_*.wiki)."""
    services = {'srv': {f'{k:04x}': v for k, v in SRV_COMMANDS.items()}}
    for page, (short, _) in SERVICE_PAGES.items():
        path = ROOT / 'out' / 'ref' / '3dbrew' / f'svc_{page}.wiki'
        text = path.read_text(encoding='utf-8', errors='replace')
        start, end = SECTION.get(page, (None, None))
        if start:
            text = text[text.index(start):]
        if end:
            text = text[:text.index(end)]
        cmds = {}
        # Table rows are separated by "|-"; cells by a new line starting
        # with "|" or by "||". A command row starts with its header word.
        for row in re.split(r'\n\|-', text):
            cells = [c.strip() for c in re.split(r'\n\||\|\|', row)]
            for i, cell in enumerate(cells[:-1]):
                m = re.fullmatch(r'\|?\s*0x([0-9A-Fa-f]{4})[0-9A-Fa-f]{4}', cell)
                if not m:
                    continue
                cid = int(m[1], 16)
                label = ''
                rest = cells[i + 1:]
                # The description is the link whose text is the command's own
                # page name ([[APT:Enable|Enable]]); else the first real text.
                for cell in rest:
                    for target, text in re.findall(r'\[\[([^|\]]+)\|?([^\]]*)\]\]', cell):
                        if target.split(':')[-1] == (text or target).split(':')[-1] and ':' in target:
                            label = target.split(':')[-1]
                            break
                    if label:
                        break
                if not label:
                    for cell in rest:
                        text = re.sub(r'\[\[[^|\]]*\|?([^\]]*)\]\]', r'\1', cell).strip()
                        if re.search('[A-Za-z]{3}', text) and text not in ('Yes', 'No', 'None'):
                            label = text
                            break
                if cid:
                    cmds.setdefault(f'{cid:04x}', label.strip()[:60])
                break
        services[short] = cmds
    SERVICES_JSON.write_text(json.dumps(services, indent=1, sort_keys=True) + '\n', newline='\n')
    print(f'wrote {SERVICES_JSON.relative_to(ROOT)}: {sum(map(len, services.values()))} commands')


def handled_cases(file, func):
    """Command ids a handler function (its `case` labels) or a stub table covers."""
    src = (ROOT / file).read_text(encoding='utf-8')
    start = src.index(func)
    body = src[start:src.index('\n}', start)] if '(' in src[start:start + len(func) + 2] else \
        src[start:src.index('};', start)]
    if '(' in src[start:start + len(func) + 2]:
        return {int(x, 16) for x in re.findall(r'case 0x([0-9A-Fa-f]+)', body)}
    return {int(x, 16) for x in re.findall(r'\{\s*0x([0-9A-Fa-f]+),', body)}


def service_groups():
    data = json.loads(SERVICES_JSON.read_text())
    groups = []
    impl_of = {short: impl for short, impl in SERVICE_PAGES.values()}
    impl_of['srv'] = [('hle/srv.c', 'srv_handle')]
    for short, cmds in data.items():
        done = set()
        for file, func in impl_of.get(short, []):
            done |= handled_cases(file, func)
        cells = [(f'{short} 0x{int(c, 16):04x} {name}', int(c, 16) in done) for c, name in sorted(cmds.items())]
        groups.append((short, cells))
    return groups


def cpu_groups():
    import capstone.arm as arm
    from recomp import translate
    handled = set(translate._HANDLERS)
    families = [
        ('data processing', ARMV6K[:21]), ('multiply', ARMV6K[21:28]), ('DSP multiply', ARMV6K[28:44]),
        ('v6 multiply', ARMV6K[44:62]), ('saturate', ARMV6K[62:70]), ('SIMD', ARMV6K[70:106]),
        ('extend', ARMV6K[106:118]), ('misc', ARMV6K[118:127]), ('load/store', ARMV6K[127:153]),
        ('exclusive', ARMV6K[153:162]), ('branch/system', ARMV6K[162:181]),
        ('coprocessor', ARMV6K[181:193]), ('hints', ARMV6K[193:200]), ('VFPv2', ARMV6K[200:]),
    ]
    groups = []
    for name, mnems in families:
        cells = []
        for m in mnems:
            ids = [getattr(arm, f'ARM_INS_{m}', None)]
            if m in ('YIELD', 'WFE', 'WFI', 'SEV'):
                ok = False
            elif m in ('VPUSH', 'VPOP'):
                ok = any(i in handled for i in ids if i is not None)
            else:
                ok = any(i in handled for i in ids if i is not None)
            cells.append((m.lower(), ok))
        groups.append((name, cells))
    return groups


def shader_groups():
    src = (ROOT / 'gpu' / 'shader.c').read_text(encoding='utf-8')
    present = {int(x, 16) for x in re.findall(r'case 0x([0-9A-Fa-f]{2})', src)}
    words = set(re.findall(r'\b([A-Z][A-Z0-9]{1,6})\b', src))
    arith, flow = [], []
    for op, name in PICA_OPCODES.items():
        ok = op in present or name in words
        (flow if op >= 0x20 and op < 0x2E else arith).append((name, ok))
    return [('arithmetic', arith), ('control flow', flow)]


def compat():
    text = (ROOT / 'docs' / 'compat.md').read_text(encoding='utf-8')
    runs = int(re.search(r'\| runs \| (\d+) \|', text)[1])
    total = sum(int(n) for n in re.findall(r'^\| [a-z-]+ \| (\d+) \|$', text, re.M))
    return runs, total


# ---------------------------------------------------------------- layout

def squarify(sizes, x, y, w, h):
    """Squarified treemap (Bruls et al.): rectangles for `sizes` (sorted large first)."""
    out = []
    items = list(sizes)
    total = sum(s for _, s in items)
    scale = w * h / total if total else 0
    items = [(k, s * scale) for k, s in items]

    def worst(row, side):
        s = sum(a for _, a in row)
        return max(max(side * side * a / (s * s), (s * s) / (side * side * a)) for _, a in row)

    while items:
        side = min(w, h)
        row = [items.pop(0)]
        while items and worst(row + [items[0]], side) <= worst(row, side):
            row.append(items.pop(0))
        s = sum(a for _, a in row)
        if w >= h:                          # a column on the left
            cw = s / h
            cy = y
            for k, a in row:
                out.append((k, x, cy, cw, a / cw))
                cy += a / cw
            x, w = x + cw, w - cw
        else:                               # a row on top
            rh = s / w
            cx = x
            for k, a in row:
                out.append((k, cx, y, a / rh, rh))
                cx += a / rh
            y, h = y + rh, h - rh
    return out


GREEN, GREY, BG, EDGE, TEXT = '#2ea043', '#5b636e', '#0d1117', '#0d1117', '#ffffff'


def cells_in(cells, x, y, w, h):
    """Lay a group's cells out as a grid filling its rectangle."""
    n = len(cells)
    cols = max(1, round((n * w / h) ** 0.5)) if h > 0 else 1
    rows = (n + cols - 1) // cols
    cw, ch = w / cols, h / rows
    svg = []
    for i, (label, ok) in enumerate(cells):
        cx, cy = x + (i % cols) * cw, y + (i // cols) * ch
        svg.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{max(cw - 1, 0.5):.1f}" height="{max(ch - 1, 0.5):.1f}" '
                   f'fill="{GREEN if ok else GREY}"><title>{escape(label)}{"" if ok else " (not yet)"}</title></rect>')
    return svg


def escape(s):
    s = ''.join(c for c in s if c >= ' ')
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def panel(title, groups, x, y, w, h):
    total = sum(len(c) for _, c in groups)
    done = sum(ok for _, c in groups for _, ok in c)
    pct = 100 * done / total if total else 0
    svg = [f'<text x="{x}" y="{y + 16}" fill="{TEXT}" font-size="16">{escape(title)}: {pct:.1f}% '
           f'({done}/{total})</text>']
    top = y + 26
    sized = sorted(((name, len(c)) for name, c in groups if c), key=lambda t: -t[1])
    cells = dict(groups)
    for name, gx, gy, gw, gh in squarify(sized, x, top, w, h - 26):
        svg.append(f'<rect x="{gx:.1f}" y="{gy:.1f}" width="{gw:.1f}" height="{gh:.1f}" fill="{EDGE}"/>')
        svg += cells_in(cells[name], gx + 1.5, gy + 1.5, gw - 3, gh - 3)
        if gw > 34 and gh > 16:
            svg.append(f'<text x="{gx + 4:.1f}" y="{gy + 14:.1f}" fill="{TEXT}" font-size="11" '
                       f'style="paint-order:stroke" stroke="#0d1117" stroke-width="2.5">{escape(name)}</text>')
    return svg, pct


def badge(x, y, label, value, color):
    lw, vw = 8 * len(label) + 16, 8 * len(value) + 16
    return [f'<rect x="{x}" y="{y}" width="{lw}" height="26" fill="#30363d"/>',
            f'<text x="{x + lw / 2}" y="{y + 18}" fill="{TEXT}" font-size="13" text-anchor="middle">{escape(label)}</text>',
            f'<rect x="{x + lw}" y="{y}" width="{vw}" height="26" fill="{color}"/>',
            f'<text x="{x + lw + vw / 2}" y="{y + 18}" fill="{TEXT}" font-size="13" text-anchor="middle">{escape(value)}</text>'], lw + vw + 10


def color_for(pct):
    return GREEN if pct >= 90 else '#9a6700' if pct >= 50 else '#b62324'


def main():
    if '--refresh-services' in sys.argv:
        refresh_services()
    W, H = 1240, 470
    body = []
    s_svg, s_pct = panel('OS services', service_groups(), 10, 50, 600, 410)
    c_svg, c_pct = panel('CPU instructions', cpu_groups(), 625, 50, 360, 410)
    g_svg, g_pct = panel('GPU shaders', shader_groups(), 1000, 50, 230, 410)
    runs, total = compat()
    x = 10
    for label, value, pct in (('services', f'{s_pct:.1f}%', s_pct), ('CPU', f'{c_pct:.1f}%', c_pct),
                              ('shaders', f'{g_pct:.1f}%', g_pct),
                              ('examples running', f'{runs}/{total}', 100 * runs / total)):
        b, width = badge(x, 10, label, value, color_for(pct))
        body += b
        x += width
    body += s_svg + c_svg + g_svg
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
           f'font-family="Segoe UI, Helvetica, Arial, sans-serif">',
           f'<rect width="{W}" height="{H}" fill="{BG}"/>'] + body + ['</svg>']
    OUT.write_text('\n'.join(svg) + '\n', newline='\n')
    print(f'wrote {OUT.relative_to(ROOT)}: services {s_pct:.1f}%, CPU {c_pct:.1f}%, shaders {g_pct:.1f}%, '
          f'examples {runs}/{total}')


if __name__ == '__main__':
    main()
