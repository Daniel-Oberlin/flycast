"""Log camera distance from the boat whenever the first-person view (mode 0) is active."""
import struct, sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem
m = DcMem(); end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 900); last = 0
while time.time() < end:
    phys = m.read_u32(0x8c1fc988)
    if 0x8c000000 <= phys < 0x8d000000 and m.read_u32(0x8c1fc98c) == 0 and time.time() - last > 2:
        wob = m.read_u32(phys + 260); pd = m.read_u32(phys + 168)
        bp = struct.unpack('<3f', m.read(wob + 92, 12)); cp = struct.unpack('<3f', m.read(0x8c1fca68, 12))
        print(time.strftime('%H:%M:%S'), 'view 0  eye x', round(m.read_f32(pd + 0x1c0), 3),
              ' |cam-boat|', round(sum((a - b) ** 2 for a, b in zip(cp, bp)) ** .5, 3), flush=True)
        last = time.time()
    time.sleep(0.3)
