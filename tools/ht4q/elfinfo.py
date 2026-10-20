"""Carve an ELF32 at an offset in a blob; list sections; dump symbols to a text file."""
import struct, sys
blob = open(sys.argv[1], 'rb').read()
off = int(sys.argv[2]); out = sys.argv[3]
h = blob[off:off + 52]
E = '<' if h[5] == 1 else '>'
(e_type, e_mach, _, e_entry, e_phoff, e_shoff, _, _, _, _, e_shentsize, e_shnum, e_shstrndx) = struct.unpack(E + 'HHIIIIIHHHHHH', h[16:52])
size = e_shoff + e_shnum * e_shentsize
elf = blob[off:off + size]
open(out + '.elf', 'wb').write(elf)
secs = [struct.unpack(E + 'IIIIIIIIII', elf[e_shoff + i * 40:e_shoff + i * 40 + 40]) for i in range(e_shnum)]
shstr = secs[e_shstrndx]
name = lambda tab, o: elf[tab + o:elf.index(b'\0', tab + o)].decode('latin1')
print(f'entry={e_entry:#x} size={size}')
for i, s in enumerate(secs):
    print(f'{i:3} {name(shstr[4], s[0]):24} type={s[1]:2} addr={s[3]:#010x} off={s[4]:8} size={s[5]:8}')
with open(out + '.syms.txt', 'w') as f:
    for s in secs:
        if s[1] == 2:  # SYMTAB
            strtab = secs[s[6]][4]
            for j in range(s[5] // 16):
                st_name, st_value, st_size, st_info, _, st_shndx = struct.unpack(E + 'IIIBBH', elf[s[4] + j * 16:s[4] + j * 16 + 16])
                f.write(f'{st_value:08x} {st_size:6} {st_info >> 4} {st_info & 15} {st_shndx:5} {name(strtab, st_name)}\n')
