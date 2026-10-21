"""Find Hydro Thunder's gamecam statics in live Dreamcast RAM.

Signature (from the debug builds' DWARF info, see phase2 notes):
    P-0xa0  Cam_t _CamXfmData: Xfm_t.mf[4][3] whose 3x3 part is a rotation (unit columns)
    P       Vec3  _Pos                then int _nHeading, _nPitch, _nRoll   (bradians)
    P+0x2c  Vec3  _DrawPos (near _Pos) then int _nDrawHeading, _nDrawPitch, _nDrawRoll
Run while racing. Takes two snapshots and prefers candidates whose _Pos moved.

    python3 -I camfind.py [--save DIR]
"""
import math, struct, sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem, RAM_BASE

def f32s(ram, off, n): return struct.unpack_from(f'<{n}f', ram, off)
def s32s(ram, off, n): return struct.unpack_from(f'<{n}i', ram, off)

def finite_vec(v, lim=1e6):
    return all(math.isfinite(x) and abs(x) < lim for x in v) and any(abs(x) > 1e-3 for x in v)

def angle_ok(a): return -0x40000 < a < 0x40000

def is_rotation(m):  # mf[col][row], cols 0..2
    cols = [m[0:3], m[3:6], m[6:9]]
    norms = [math.sqrt(sum(x * x for x in c)) for c in cols]
    if not all(abs(n - 1) < 0.02 for n in norms): return False
    dots = [sum(a * b for a, b in zip(cols[i], cols[j])) for i, j in ((0, 1), (0, 2), (1, 2))]
    return all(abs(d) < 0.02 for d in dots)

def candidates(ram):
    out = []
    for p in range(0xa0, len(ram) - 0x40, 4):
        pos = f32s(ram, p, 3)
        if not finite_vec(pos): continue
        if not all(angle_ok(a) for a in s32s(ram, p + 12, 3)): continue
        dpos = f32s(ram, p + 0x2c, 3)
        if not finite_vec(dpos) or math.dist(pos, dpos) > 100: continue
        if not all(angle_ok(a) for a in s32s(ram, p + 0x38, 3)): continue
        rot_ok = is_rotation(f32s(ram, p - 0xa0, 9))
        out.append((p, pos, dpos, rot_ok))
    return out

def main():
    save = sys.argv[sys.argv.index('--save') + 1] if '--save' in sys.argv else None
    m = DcMem()
    t = time.time(); a = m.ram(); print(f'snapshot 1: {time.time() - t:.1f}s')
    time.sleep(0.5)
    b = m.ram()
    if save:
        os.makedirs(save, exist_ok=True)
        open(os.path.join(save, 'ram_a.bin'), 'wb').write(a)
        open(os.path.join(save, 'ram_b.bin'), 'wb').write(b)
    hits = []
    for p, pos, dpos, rot_ok in candidates(a):
        pos2 = f32s(b, p, 3)
        moved = math.dist(pos, pos2)
        hits.append((rot_ok, moved > 1e-3, p, pos, dpos, moved))
    hits.sort(key=lambda h: (h[0], h[1]), reverse=True)
    print(f'{len(hits)} candidates; rotation-matrix-before & moving first:')
    for rot_ok, mv, p, pos, dpos, moved in hits[:15]:
        print(f'  _Pos @ {RAM_BASE + p:#010x}  rot={"Y" if rot_ok else "n"}  moved={moved:8.3f}  '
              f'pos=({pos[0]:9.2f},{pos[1]:9.2f},{pos[2]:9.2f})  '
              f'ang={s32s(a, p + 12, 3)}  drawang={s32s(a, p + 0x38, 3)}')

if __name__ == '__main__':
    main()
