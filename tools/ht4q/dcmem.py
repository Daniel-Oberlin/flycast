"""Client for the fork's ht4q memserver (core/ht4q/memserver.cpp).

Launch Flycast with HT4Q_MEMSERVER=3264 in the environment. Reads/writes emulated main
RAM while the game keeps running (never halts the emulator).
    from dcmem import DcMem
    m = DcMem(); ram = m.ram(); m.write_f32(addr, 1.0)
"""
import socket, struct

RAM_BASE, RAM_SIZE = 0x8c000000, 0x1000000

# Dreamcast controller buttons (core/input/gamepad.h)
BTN_C, BTN_B, BTN_A, BTN_START = 1 << 0, 1 << 1, 1 << 2, 1 << 3
DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT = 1 << 4, 1 << 5, 1 << 6, 1 << 7
BTN_Y, BTN_X = 1 << 9, 1 << 10


class DcMem:
    def __init__(self, host='127.0.0.1', port=3264, timeout=10):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def _recv(self, n):
        out = bytearray()
        while len(out) < n:
            chunk = self.s.recv(n - len(out))
            if not chunk:
                raise ConnectionError('memserver closed the connection')
            out += chunk
        return bytes(out)

    def read(self, addr, length):
        self.s.sendall(b'R' + struct.pack('<II', addr, length))
        return self._recv(length)

    def write(self, addr, data):
        self.s.sendall(b'W' + struct.pack('<II', addr, len(data)) + data)
        if self._recv(1) != b'\0':
            raise IOError(f'write {addr:#x}: out of range or emulator not running')

    def patch(self, addr, data):
        """Write applied on the emulator thread at the next maple poll (safe for code pages)."""
        self.s.sendall(b'P' + struct.pack('<II', addr, len(data)) + data)
        if self._recv(1) != b'\0':
            raise IOError(f'patch {addr:#x}: out of range or emulator not running')

    def input(self, pressed=0, lt=0, rt=0, x=0, y=0, active=True):
        """Virtual controller on port A. pressed = OR of BTN_* bits; triggers 0..0xffff; stick -32768..32767."""
        self.s.sendall(b'I' + struct.pack('<II', 0, 16) + struct.pack('<IIHHhh', int(active), pressed, lt, rt, x, y))
        self._recv(1)

    def release(self):
        self.input(active=False)

    def read_u32(self, addr): return struct.unpack('<I', self.read(addr, 4))[0]
    def read_f32(self, addr): return struct.unpack('<f', self.read(addr, 4))[0]
    def write_u32(self, addr, v): self.write(addr, struct.pack('<I', v))
    def write_f32(self, addr, v): self.write(addr, struct.pack('<f', v))

    def ram(self):
        return self.read(RAM_BASE, RAM_SIZE)
