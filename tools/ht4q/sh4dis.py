"""SH-4 disassembler driven by Flycast's own opcode table (core/hw/sh4/sh4_opcode_list.cpp).

    from sh4dis import Sh4Dis
    d = Sh4Dis(); text, ref = d.dis(opcode, pc)   # ref = PC-relative literal address or branch target

Also a CLI over live RAM:   python3 -I sh4dis.py ADDR [COUNT]
"""
import os, re, struct, sys

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../core/hw/sh4/sh4_opcode_list.cpp')

def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

class Sh4Dis:
    def __init__(self, table=TABLE):
        src = open(table).read()
        masks = {m: int(v, 16) for m, v in re.findall(r'#define (Mask_\w+) (0x[0-9A-Fa-f]+)', src)}
        body = src[src.index('opcodes[]'):]
        self.ops = []
        for mask, key, diss in re.findall(r'\{[^{}\n]*?,\s*(Mask_\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*\w+\s*,\s*"([^"]*)"', body):
            self.ops.append((masks[mask], int(key, 16), diss))
        # opcode -> first matching entry, like Flycast's lookup build
        self.lut = [None] * 0x10000
        for mask, key, diss in self.ops:
            for op in self._expand(mask, key):
                if self.lut[op] is None:
                    self.lut[op] = diss

    @staticmethod
    def _expand(mask, key):
        free = [b for b in range(16) if not mask & (1 << b)]
        for i in range(1 << len(free)):
            v = key
            for j, b in enumerate(free):
                if i & (1 << j):
                    v |= 1 << b
            yield v

    def dis(self, op, pc):
        t = self.lut[op]
        if t is None:
            return f'.word 0x{op:04x}', None
        n, m = (op >> 8) & 15, (op >> 4) & 15
        i4, i8, i12 = op & 15, op & 0xff, op & 0xfff
        ref = None
        def sub(mo):
            nonlocal ref
            tag = mo.group(1)
            if tag == 'REG_N': return f'R{n}'
            if tag == 'REG_M': return f'R{m}'
            if tag in ('FREG_N',): return f'FR{n}'
            if tag in ('FREG_M', 'FREG_M_SD_F'): return f'FR{m}'
            if tag == 'FREG_0': return 'FR0'
            if tag == 'RM_BANK': return f'R{m & 7}_BANK'
            if tag in ('DFREG_N', 'DR_N'): return f'DR{n >> 1}'
            if tag == 'DFREG_M': return f'DR{m >> 1}'
            if tag == 'XDFREG_N': return (f'XD{n >> 1}' if n & 1 else f'DR{n >> 1}')
            if tag == 'XDFREG_M': return (f'XD{m >> 1}' if m & 1 else f'DR{m >> 1}')
            if tag == 'FV_N': return f'FV{n & 12}'
            if tag == 'FV_M': return f'FV{(n & 3) * 4}'
            if tag == 'disp4b': return f'0x{i4:X}'
            if tag == 'disp4w': return f'0x{i4 * 2:X}'
            if tag == 'disp4dw': return f'0x{i4 * 4:X}'
            if tag == 'PCdisp8w':
                ref = pc + 4 + (i8 << 1); return f'0x{ref:X}[PC]'
            if tag == 'PCdisp8d':
                ref = (pc & ~3) + 4 + (i8 << 2); return f'0x{ref:X}[PC]'
            if tag == 'disp8b': return f'0x{i8:X}'
            if tag == 'disp8w': return f'0x{i8 * 2:X}'
            if tag == 'disp8dw': return f'0x{i8 * 4:X}'
            if tag == 'GBRdisp8b': return f'0x{i8:X}+GBR'
            if tag == 'GBRdisp8w': return f'0x{i8 * 2:X}+GBR'
            if tag == 'GBRdisp8dw': return f'0x{i8 * 4:X}+GBR'
            if tag == 'bdisp8':
                ref = pc + 4 + _sext(i8, 8) * 2; return f'0x{ref:X}'
            if tag == 'bdisp12':
                ref = pc + 4 + _sext(i12, 12) * 2; return f'0x{ref:X}'
            if tag == 'imm8': return f'0x{i8:X}'
            if tag == 'simm8': return f'{_sext(i8, 8)}'
            if tag == 'simm8hex': return f'0x{_sext(i8, 8) & 0xffffffff:X}' if _sext(i8, 8) < 0 else f'0x{i8:X}'
            return '<' + tag + '>'
        return re.sub(r'<(\w+)>', sub, t), ref

    def listing(self, mem, base, start, count, annotate=None):
        """mem: bytes of RAM starting at `base`. Yields text lines; annotates PC-relative literals."""
        out = []
        for k in range(count):
            pc = start + 2 * k
            op = struct.unpack_from('<H', mem, pc - base)[0]
            text, ref = self.dis(op, pc)
            note = ''
            if ref is not None and 'PC]' in text:
                if 'mov.l' in text:
                    val = struct.unpack_from('<I', mem, ref - base)[0]
                    note = f'  ; =0x{val:08X}' + (f' {annotate(val)}' if annotate and annotate(val) else '')
                else:
                    note = f'  ; =0x{struct.unpack_from("<H", mem, ref - base)[0]:04X}'
            out.append(f'{pc:08X}: {op:04X}  {text}{note}')
        return out

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from dcmem import DcMem, RAM_BASE
    addr = int(sys.argv[1], 16); count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    ram = DcMem().ram()
    print('\n'.join(Sh4Dis().listing(ram, RAM_BASE, addr, count)))
