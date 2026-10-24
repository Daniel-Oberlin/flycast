"""Drive Hydro Thunder through the virtual controller and take screenshots.

    python3 -I drive.py STEP...
Steps:  tap:BTN[:secs]   press and release (BTN = START A B X Y UP DOWN LEFT RIGHT)
        hold:BTN:secs    hold a button
        gas:secs[:steer] hold throttle (right trigger + A) with optional stick X (-1..1)
        gason[:steer] / gasoff   throttle on until gasoff (other steps run meanwhile)
        eye:X[:Y[:Z]]    set PhysData eye vector (default 0.2005:7.51979:3.75)
        torace           press START/A until a race is running
        to2p             from the title screen into a 2-player Thunder Park race (needs a 2nd controller:
                         -config input:device2=0 before the game path)
        view:N           press Y until camera view mode N (0 first-person, 1 low, 2 high)
        hook, head:X:Y:Z:YAW:PITCH:ROLL | head:off, campos   camera hook status / head pose override
        wait:secs
        shot:NAME        rendered frame -> $SHOTDIR/NAME.png (needs HT4Q_FRAMEDUMP=$SHOTDIR/frame.txt)
        dump:NAME        frame polygon list -> $SHOTDIR/NAME.txt (needs HT4Q_FRAMEDUMP=$SHOTDIR/frame.txt)
        cam              print camera view mode / distance to boat
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcmem, camhook
from dcmem import DcMem

BTN = dict(START=dcmem.BTN_START, A=dcmem.BTN_A, B=dcmem.BTN_B, X=dcmem.BTN_X, Y=dcmem.BTN_Y,
           UP=dcmem.DPAD_UP, DOWN=dcmem.DPAD_DOWN, LEFT=dcmem.DPAD_LEFT, RIGHT=dcmem.DPAD_RIGHT)
SHOTDIR = os.environ.get('SHOTDIR', '.')

def cam(m):
    phys, view = m.read_u32(0x8c1fc988), m.read_u32(0x8c1fc98c)
    if not 0x8c000000 <= phys < 0x8d000000:
        return f'view={view} no boat'
    wob, pd = m.read_u32(phys + 260), m.read_u32(phys + 168)
    bp = struct.unpack('<3f', m.read(wob + 92, 12)); cp = struct.unpack('<3f', m.read(0x8c1fca68, 12))
    d = sum((a - b) ** 2 for a, b in zip(cp, bp)) ** .5
    return f'view={view} |cam-boat|={d:.3f} eye={tuple(round(v, 3) for v in struct.unpack("<3f", m.read(pd + 0x1c0, 12)))}'

def main():
    m = DcMem()
    held = dict(pressed=0)
    try:
        for step in sys.argv[1:]:
            k, *a = step.split(':')
            if k == 'tap':
                h = dict(held); h['pressed'] = h.get('pressed', 0) | BTN[a[0]]
                m.input(**h); time.sleep(float(a[1]) if len(a) > 1 else 0.4); m.input(**held); time.sleep(0.25)
            elif k == 'hold':
                m.input(BTN[a[0]]); time.sleep(float(a[1])); m.input(0)
            elif k == 'gas':
                x = int(float(a[1]) * 32767) if len(a) > 1 else 0
                m.input(dcmem.BTN_A, rt=0xffff, x=x); time.sleep(float(a[0])); m.input(0)
            elif k == 'gason':     # throttle stays on across later steps (taps keep it held)
                held = dict(pressed=dcmem.BTN_A, rt=0xffff, x=int(float(a[0]) * 32767) if a else 0)
                m.input(**held)
            elif k == 'gasoff':
                held = dict(pressed=0); m.input(0)
            elif k == 'eye':       # eye:X[:Y[:Z]] set PhysData eye vector of the attached boat
                pd = m.read_u32(m.read_u32(0x8c1fc988) + 168)
                for i, v in enumerate(a):
                    m.write_f32(pd + 0x1c0 + 4 * i, float(v))
            elif k == 'poke':      # poke:ADDR:FLOAT
                m.write_f32(int(a[0], 16), float(a[1]))
            elif k == 'torace':    # press START/A until a boat is attached to the camera (max 60 s)
                end = time.time() + 60
                while time.time() < end and not 0x8c000000 <= m.read_u32(0x8c1fc988) < 0x8d000000:
                    for b in ('START', 'A'):
                        m.input(BTN[b]); time.sleep(0.4); m.input(0); time.sleep(1.2)
                print('race:', cam(m), flush=True)
            elif k == 'to2p':      # main menu RIGHT = 2 players; player 2 joins with START, both pick with A
                for b, port, secs in (('START', 0, 7), ('RIGHT', 0, 3), ('START', 1, 3), ('A', 1, 5), ('A', 0, 5),
                                      ('A', 1, 2), ('A', 0, 4), ('A', 1, 4), ('A', 0, 3), ('A', 1, 1)):
                    m.input(BTN[b], port=port); time.sleep(0.15); m.release(); time.sleep(secs)
                end = time.time() + 30
                while time.time() < end and not 0x8c000000 <= m.read_u32(0x8c1fc988) < 0x8d000000:
                    time.sleep(0.5)
                humans = m.read_u32(0x8c25978c)
                print('race: humans', humans, cam(m), '' if humans == 2 else '(not 2 players: menu timing missed)', flush=True)
            elif k == 'view':      # cycle with Y until the given view mode (0 = first-person)
                for _ in range(4):
                    if m.read_u32(0x8c1fc98c) == int(a[0]): break
                    h = dict(held); h['pressed'] = h.get('pressed', 0) | BTN['Y']
                    m.input(**h); time.sleep(0.4); m.input(**held); time.sleep(0.6)
            elif k == 'hook':      # print camera hook status
                print('hook', camhook.status(m), camhook.pose_block(m), flush=True)
            elif k == 'head':      # head:X:Y:Z:YAW:PITCH:ROLL (x right, y up, z back; degrees, +yaw left, +pitch up) | head:off
                if a[0] == 'off': m.head(active=False)
                else: m.head(*(float(v) for v in a + ['0'] * (6 - len(a))))
            elif k == 'campos':    # rendered camera pos (Cam_t.pos) minus _DrawPos
                cp = struct.unpack('<3f', m.read(0x8c1fca08, 12)); dp = struct.unpack('<3f', m.read(0x8c1fca68, 12))
                print('cam-drawpos', tuple(round(x - y, 3) for x, y in zip(cp, dp)),
                      'drawhdg', m.read_u32(0x8c1fca74), 'camrot', struct.unpack('<3i', m.read(0x8c1fc99c + 144, 12)), flush=True)
            elif k == 'wait':
                time.sleep(float(a[0]))
            elif k == 'shot':      # rendered frame from Flycast itself (never the screen)
                req = os.path.join(SHOTDIR, 'frame.txt.shot.req'); png = os.path.join(SHOTDIR, 'frame.txt.png')
                open(req, 'w').close()
                end = time.time() + 3
                while os.path.exists(req) and time.time() < end: time.sleep(0.05)
                time.sleep(0.2)
                out = os.path.join(SHOTDIR, a[0] + '.png')
                if os.path.exists(req): os.remove(req); print('shot failed (Flycast needs HT4Q_FRAMEDUMP=$SHOTDIR/frame.txt)', flush=True)
                else: os.replace(png, out); print('shot', out, flush=True)
            elif k == 'dump':      # dump:NAME  frame polygon list (Flycast run with HT4Q_FRAMEDUMP=$SHOTDIR/frame.txt)
                open(os.path.join(SHOTDIR, 'frame.txt.req'), 'w').close(); time.sleep(1.0)
                os.replace(os.path.join(SHOTDIR, 'frame.txt'), os.path.join(SHOTDIR, a[0] + '.txt'))
                print('dump', a[0], flush=True)
            elif k == 'cam':
                print(cam(m), flush=True)
    finally:
        m.release()

if __name__ == '__main__':
    main()
