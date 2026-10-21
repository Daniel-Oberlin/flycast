"""Phase 3 control test: shift the drawn camera sideways by hammering memory writes.

The game recomputes the camera every frame, so we overwrite continuously (~thousands/s).
Sequence: 2s none, 2s +OFFSET right, 2s none, 2s -OFFSET (left), 2s none.

    python3 -I campoke.py [offset=3.0] [target=drawpos|xfm]
      drawpos: write _DrawPos (input to the camera build)
      xfm:     write Cam_t.pos and the forward matrix translation (output of the build)
"""
import struct, sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem

# Retail Hydro Thunder (USA) (Rev 1) gamecam statics
CAM = 0x8c1fc99c          # Cam_t _CamXfmData: Xfm_t xfm (+0), pos (+108)
POS = 0x8c1fca3c          # Vec3 _Pos
DRAWPOS = 0x8c1fca68      # Vec3 _DrawPos

def wait_for_race(m, min_speed=5.0):
    """Block until the camera position moves faster than min_speed units/s."""
    print('waiting for the boat to move...', flush=True)
    last = struct.unpack('<3f', m.read(POS, 12))
    while True:
        time.sleep(0.5)
        cur = struct.unpack('<3f', m.read(POS, 12))
        if sum((a - b) ** 2 for a, b in zip(cur, last)) ** 0.5 / 0.5 > min_speed:
            print('racing detected', flush=True)
            return
        last = cur

def main():
    off = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
    target = sys.argv[2] if len(sys.argv) > 2 else 'drawpos'
    cycle = '--cycle' in sys.argv
    m = DcMem()
    if cycle:
        wait_for_race(m)
        phases = [(off, 3), (0, 3), (-off, 3), (0, 3)] * 5
    else:
        phases = [(0, 2), (off, 4), (0, 3), (-off, 4), (0, 2)]
    for shift, secs in phases:
        print(f'shift {shift:+.1f} for {secs}s', flush=True)
        end = time.time() + secs
        n = 0
        while time.time() < end:
            if shift == 0:
                time.sleep(0.01)
                continue
            pos = struct.unpack('<3f', m.read(POS, 12))
            # reverse transform (camera->world) column 0 = camera right axis in world space
            right = struct.unpack('<3f', m.read(CAM + 48, 12))
            new = struct.pack('<3f', *(p + shift * r for p, r in zip(pos, right)))
            if target == 'drawpos':
                m.write(DRAWPOS, new)
            else:
                # Forward (world->camera) matrix mf[col][row]; camera-space x translation is
                # mf[3][0] = -(mf[0][0]*px + mf[1][0]*py + mf[2][0]*pz). Recompute from Cam_t.pos
                # each time (idempotent), then move the camera right by `shift`.
                mf = struct.unpack('<12f', m.read(CAM, 48))
                cp = struct.unpack('<3f', m.read(CAM + 108, 12))
                tx = -(mf[0] * cp[0] + mf[3] * cp[1] + mf[6] * cp[2])
                m.write(CAM + 36, struct.pack('<f', tx - shift))
            n += 1
        if shift:
            print(f'   {n / secs:.0f} writes/s')

if __name__ == '__main__':
    main()
