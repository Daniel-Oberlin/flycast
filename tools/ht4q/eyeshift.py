"""Hold the first-person eye offset shifted sideways (PhysData +0x1c0 = eye Vec3, x = sideways).

Re-applies once per second for DURATION seconds so new races / boat changes keep the shift,
then restores every PhysData it touched.     python3 -I eyeshift.py [shift=3.0] [duration=180]
"""
import struct, sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem

PHYS_PTR, VIEW = 0x8c1fc988, 0x8c1fc98c
PHYSDATA, EYE = 168, 0x1c0
KNOWN_ORIG_X = {0x8cd5e7c0: 0.20052799582481384}   # captured before the first shift

def main():
    shift = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 180
    m = DcMem()
    orig = dict(KNOWN_ORIG_X)
    end = time.time() + duration
    try:
        while time.time() < end:
            phys = m.read_u32(PHYS_PTR)
            if 0x8c000000 <= phys < 0x8d000000:
                pd = m.read_u32(phys + PHYSDATA)
                if 0x8c000000 <= pd < 0x8d000000:
                    x = m.read_f32(pd + EYE)
                    if pd not in orig:
                        orig[pd] = x
                        print(f'new boat params {pd:#x}, eye x {x:.3f}', flush=True)
                    if abs(x - (orig[pd] + shift)) > 1e-4:
                        m.write_f32(pd + EYE, orig[pd] + shift)
                        print(f'applied shift to {pd:#x} (view {m.read_u32(VIEW)})', flush=True)
            time.sleep(1)
    finally:
        for pd, x in orig.items():
            m.write_f32(pd + EYE, x)
        print('restored', [hex(p) for p in orig], flush=True)

if __name__ == '__main__':
    main()
