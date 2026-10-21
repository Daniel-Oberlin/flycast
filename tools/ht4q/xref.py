"""Find code that loads addresses in [LO, HI) via mov.l @(disp,PC) literals.

    python3 -I xref.py LO HI [code_start code_end]
Prints each referencing instruction with its function start (nearest preceding 'sts.l PR,@-R15').
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem, RAM_BASE
from sh4dis import Sh4Dis

NAMES = {
    0x8c1fc97c: 'Gamecam_pSector', 0x8c1fc988: '_pAttachedPhys', 0x8c1fc98c: '_nViewMode',
    0x8c1fc99c: '_CamXfmData', 0x8c1fca08: '_CamXfmData.pos', 0x8c1fca3c: '_Pos',
    0x8c1fca48: '_nHeading', 0x8c1fca4c: '_nPitch', 0x8c1fca50: '_nRoll',
    0x8c1fca54: '_nSeasickPitch', 0x8c1fca58: '_nSeasickRoll', 0x8c1fca5c: '_SweepYZ',
    0x8c1fca64: '_fSweepPitch', 0x8c1fca68: '_DrawPos', 0x8c1fca74: '_nDrawHeading',
    0x8c1fca78: '_nDrawPitch', 0x8c1fca7c: '_nDrawRoll',
}

def name(v):
    if v in NAMES: return NAMES[v]
    base = max((a for a in NAMES if a <= v), default=None)
    return f'{NAMES[base]}+0x{v - base:x}' if base and v - base < 0x40 else ''

def func_start(ram, pc, d, limit=0x2000):
    for p in range(pc, max(pc - limit, RAM_BASE), -2):
        if struct.unpack_from('<H', ram, p - RAM_BASE)[0] == 0x4f22:   # sts.l PR,@-R15
            return p
    return None

def main():
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    cs = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x8c010000
    ce = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x8c1f0000
    ram = DcMem().ram(); d = Sh4Dis()
    funcs = {}
    for pc in range(cs, ce, 2):
        op = struct.unpack_from('<H', ram, pc - RAM_BASE)[0]
        if op & 0xf000 != 0xd000:          # mov.l @(disp,PC),Rn
            continue
        lit = (pc & ~3) + 4 + ((op & 0xff) << 2)
        val = struct.unpack_from('<I', ram, lit - RAM_BASE)[0]
        if lo <= val < hi:
            f = func_start(ram, pc, d)
            funcs.setdefault(f, []).append((pc, val))
    for f, refs in sorted(funcs.items(), key=lambda x: x[0] or 0):
        print(f'func {f:#010x}' if f else 'func ?', ' '.join(sorted({name(v) or hex(v) for _, v in refs})))
        for pc, v in refs:
            print(f'    {pc:#010x} -> {v:#010x} {name(v)}')

if __name__ == '__main__':
    main()
