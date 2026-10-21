"""Status/control of the fork's camera head-pose hook (core/ht4q/camhook.cpp).

Flycast installs the hook by itself once the gamecam module of Hydro Thunder (USA) (Rev 1) is
loaded; the head pose comes from the mouse unless overridden here via the memserver.

    python3 -I camhook.py status
    python3 -I camhook.py head X Y Z YAW PITCH ROLL     # x right, y up, z back; degrees
    python3 -I camhook.py mouse                         # drop the override
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem

CALL_LITERAL = 0x8c0458b4      # literal holding BuildCam's address, used by the jsr in 0x8c045850
BUILDCAM = 0x8c093d44
STUB = 0x8c00b900
POSE = STUB + 0x40             # f32 dx dy dz, s32 dh, dp, pitchMask, dr, rollMask (world, bradians)

def status(m):
    lit = m.read_u32(CALL_LITERAL)
    return 'installed' if lit == STUB else 'not installed' if lit == BUILDCAM else f'unexpected literal {lit:#x}'

def pose_block(m):
    return struct.unpack('<3f2iIiI', m.read(POSE, 32))

if __name__ == '__main__':
    m = DcMem()
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'status'
    if cmd == 'head':
        m.head(*(float(x) for x in sys.argv[2:8]))
    elif cmd == 'mouse':
        m.head(active=False)
    import time; time.sleep(0.2)
    print(status(m), pose_block(m))
