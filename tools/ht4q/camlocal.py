"""Log the camera position in the attached boat's local frame (finds the first-person eye offset).

Waits until a boat is attached and moving, then samples for 20 s. Prints view mode, and the
camera-minus-boat vector expressed with both matrix conventions (R^T d and R d); the one that
stays constant in a given view is the eye offset.
"""
import struct, sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem

PHYS_PTR, VIEW, DRAWPOS = 0x8c1fc988, 0x8c1fc98c, 0x8c1fca68

def boat(m):
    phys = m.read_u32(PHYS_PTR)
    if not (0x8c000000 <= phys < 0x8d000000): return None
    wob = m.read_u32(phys + 260)
    if not (0x8c000000 <= wob < 0x8d000000): return None
    return struct.unpack('<3f', m.read(wob + 92, 12)), struct.unpack('<9f', m.read(wob + 104, 36))

m = DcMem()
print('waiting for an attached, moving boat...', flush=True)
last = None
while True:
    b = boat(m)
    if b and last and sum((x - y) ** 2 for x, y in zip(b[0], last)) ** 0.5 > 2:
        break
    last = b[0] if b else None
    time.sleep(0.5)
print('racing detected', flush=True)
end = time.time() + 20
while time.time() < end:
    b = boat(m)
    if not b: time.sleep(0.25); continue
    bp, R = b
    cp = struct.unpack('<3f', m.read(DRAWPOS, 12))
    d = [c - p for c, p in zip(cp, bp)]
    A = [sum(R[r * 3 + c] * d[r] for r in range(3)) for c in range(3)]
    B = [sum(R[c * 3 + r] * d[r] for r in range(3)) for c in range(3)]
    print(f'view={m.read_u32(VIEW)} |d|={sum(x * x for x in d) ** .5:6.2f} '
          f'RtD=({A[0]:6.2f},{A[1]:6.2f},{A[2]:6.2f}) RD=({B[0]:6.2f},{B[1]:6.2f},{B[2]:6.2f})', flush=True)
    time.sleep(0.25)
