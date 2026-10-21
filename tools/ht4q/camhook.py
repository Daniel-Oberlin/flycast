"""Camera head-offset hook for retail Hydro Thunder (USA) (Rev 1), installed through the memserver.

The gamecam setup at 0x8c045850 calls BuildCam(cam R4, heading R5, pitch R6, roll R7,
x FR4, y FR5, z FR6) through the literal at 0x8c0458b4. We point that literal at a stub in
unused IP.BIN RAM that adds a pose offset block to the arguments, then jumps to BuildCam.

Pose block (POSE): f32 dx, dy, dz (world space), s32 dheading, dpitch, droll (bradians).

    python3 -I camhook.py install | uninstall | status
    python3 -I camhook.py pose DX DY DZ DH DP DR
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem

CALL_LITERAL = 0x8c0458b4      # literal holding BuildCam's address, used by the jsr in 0x8c045850
BUILDCAM = 0x8c093d44
STUB = 0x8c00b900              # IP.BIN area: zero during gameplay, not dynarec write-protected
POSE = STUB + 0x40

def stub_code():
    words = [
        0xD007,     # 00 mov.l @(L_POSE,PC),R0
        0xF009,     # 02 fmov.s @R0+,FR0
        0xF400,     # 04 fadd FR0,FR4      x
        0xF009,     # 06 fmov.s @R0+,FR0
        0xF500,     # 08 fadd FR0,FR5      y
        0xF009,     # 0a fmov.s @R0+,FR0
        0xF600,     # 0c fadd FR0,FR6      z
        0x6106,     # 0e mov.l @R0+,R1
        0x351C,     # 10 add R1,R5         heading
        0x6106,     # 12 mov.l @R0+,R1
        0x361C,     # 14 add R1,R6         pitch
        0x6106,     # 16 mov.l @R0+,R1
        0x371C,     # 18 add R1,R7         roll
        0xD002,     # 1a mov.l @(L_ORIG,PC),R0
        0x402B,     # 1c jmp @R0
        0x0009,     # 1e nop
    ]
    return struct.pack('<16H', *words) + struct.pack('<II', POSE, BUILDCAM)   # 20 L_POSE, 24 L_ORIG

def set_pose(m, dx=0.0, dy=0.0, dz=0.0, dh=0, dp=0, dr=0):
    m.write(POSE, struct.pack('<3f3i', dx, dy, dz, int(dh), int(dp), int(dr)))

def status(m):
    lit = m.read_u32(CALL_LITERAL)
    return 'installed' if lit == STUB else 'not installed' if lit == BUILDCAM else f'unexpected literal {lit:#x}'

def install(m):
    lit = m.read_u32(CALL_LITERAL)
    if lit == STUB:
        return
    if lit != BUILDCAM:
        raise RuntimeError(f'literal at {CALL_LITERAL:#x} is {lit:#x}, expected {BUILDCAM:#x} (wrong disc build?)')
    if m.read(STUB, 0x60) != b'\0' * 0x60 and m.read(STUB, 0x28) != stub_code():
        raise RuntimeError('stub area is not free')
    set_pose(m)
    m.write(STUB, stub_code())
    m.patch(CALL_LITERAL, struct.pack('<I', STUB))

def uninstall(m):
    if m.read_u32(CALL_LITERAL) == STUB:
        m.patch(CALL_LITERAL, struct.pack('<I', BUILDCAM))

if __name__ == '__main__':
    m = DcMem()
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'status'
    if cmd == 'install': install(m)
    elif cmd == 'uninstall': uninstall(m)
    elif cmd == 'pose':
        v = sys.argv[2:]
        set_pose(m, *(float(x) for x in v[:3]), *(int(x) for x in v[3:6]))
    import time; time.sleep(0.2)
    print(status(m))
