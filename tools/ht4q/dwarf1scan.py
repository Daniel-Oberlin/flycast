"""Scan a raw blob for big-endian DWARF v1 DIEs (Hitachi SHC) without relying on file structure.

Each DIE: u32 length, u16 tag, attributes (u16 name<<4|form, value) filling the length.
Every SHC DIE starts with AT_sibling (0x0012); a DIE's section offset is recovered from a
per-sector base estimate (pos + len - sibling for childless DIEs).
Output: pickle list of dict(pos, off, tag, attrs).
"""
import re, struct, sys, pickle
from collections import Counter

FORM_FIXED = {1: 4, 2: 4, 5: 2, 6: 4, 7: 8}

def parse(d, p):
    ln, tag = struct.unpack_from('>IH', d, p)
    if not (8 <= ln <= 8192) or p + ln > len(d):
        return None
    q, end, attrs = p + 6, p + ln, {}
    while q < end:
        if q + 2 > end: return None
        a = struct.unpack_from('>H', d, q)[0]; q += 2
        form, name = a & 15, a >> 4
        if form in FORM_FIXED:
            n = FORM_FIXED[form]
            if q + n > end: return None
            v = d[q:q + n]; q += n
            v = int.from_bytes(v, 'big') if n <= 4 else v
        elif form == 3:
            n = struct.unpack_from('>H', d, q)[0]; q += 2
            v = d[q:q + n]; q += n
        elif form == 4:
            n = struct.unpack_from('>I', d, q)[0]; q += 4
            v = d[q:q + n]; q += n
        elif form == 8:
            z = d.find(b'\0', q, end)
            if z < 0: return None
            v = d[q:z].decode('latin1'); q = z + 1
        else:
            return None
        attrs[name] = v
    if q != end or not isinstance(attrs.get(1), int):
        return None
    return dict(pos=p, len=ln, tag=tag, attrs=attrs)

d = open(sys.argv[1], 'rb').read()
cand = re.compile(rb'\x00\x00[\x00-\x1f][\x08-\xff]\x00[\x01-\x22]\x00\x12', re.S)
dies = []
for m in cand.finditer(d):
    r = parse(d, m.start())
    if r: dies.append(r)
# per-sector section-base estimate
by_sec = {}
for r in dies:
    by_sec.setdefault(r['pos'] >> 11, Counter())[r['pos'] + r['len'] - r['attrs'][1]] += 1
for r in dies:
    base = by_sec[r['pos'] >> 11].most_common(1)[0][0]
    r['off'] = r['pos'] - base
pickle.dump(dies, open(sys.argv[2], 'wb'))
print(len(dies), 'DIEs;', Counter(r['tag'] for r in dies).most_common(12))
