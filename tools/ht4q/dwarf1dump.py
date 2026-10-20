"""Dump struct layouts / global variables from dwarf.pkl (see dwarf1scan.py)."""
import pickle, struct, sys, bisect
from collections import defaultdict

dies = pickle.load(open(sys.argv[1], 'rb'))
dies.sort(key=lambda r: r['pos'])
POS = [r['pos'] for r in dies]
by_off = defaultdict(list)
for r in dies:
    by_off[r['off']].append(r)

FUND = {1: 'char', 2: 's8', 3: 'u8', 4: 'short', 5: 's16', 6: 'u16', 7: 'int', 8: 's32', 9: 'u32',
        0xa: 'long', 0xb: 'slong', 0xc: 'ulong', 0xd: 'void*', 0xe: 'f32', 0xf: 'f64', 0x14: 'void'}
MOD = {1: '*', 2: '&', 3: 'const ', 4: 'volatile '}
T_ARRAY, T_ENUM, T_PTR, T_STRUCT, T_SUBR, T_TYPEDEF, T_UNION = 1, 4, 0xf, 0x13, 0x15, 0x16, 0x17

def resolve(ref, near):
    c = by_off.get(ref)
    return min(c, key=lambda r: abs(r['pos'] - near)) if c else None

def tname(r, near, depth=0):
    """Type name of an entity DIE r (member/variable/typedef)."""
    a = r['attrs']
    if 5 in a: return FUND.get(a[5], f'ft{a[5]:#x}')
    if 6 in a:
        b = a[6]; mods = ''.join(MOD.get(x, '?') for x in b[:-2])
        return FUND.get(int.from_bytes(b[-2:], 'big'), '?') + mods[::-1]
    if 7 in a: return udt(a[7], near, depth)
    if 8 in a:
        b = a[8]; mods = ''.join(MOD.get(x, '?') for x in b[:-4])
        return udt(int.from_bytes(b[-4:], 'big'), near, depth) + mods[::-1]
    return '?'

def udt(ref, near, depth=0):
    t = resolve(ref, near)
    if not t: return f'<ref {ref:#x}>'
    a = t['attrs']
    if isinstance(a.get(3), str) and a[3]: return a[3]
    if t['tag'] == T_ARRAY and depth < 4:
        return array_name(t, depth)
    if t['tag'] == T_PTR: return 'ptr'
    if t['tag'] == T_SUBR: return 'fn'
    return f'<anon tag{t["tag"]:#x} @{ref:#x} size={a.get(11, "?")}>'

def array_name(t, depth):
    b = t['attrs'].get(10, b'')
    # subscr_data: FMT_FT_C_C (0) : u16 fund type, u32 lo, u32 hi ; FMT_ET (8): element type attr
    dims, q, elem = [], 0, '?'
    while q < len(b):
        fmt = b[q]; q += 1
        if fmt == 0:
            q += 2; lo, hi = struct.unpack_from('>II', b, q); q += 8; dims.append(hi - lo + 1)
        elif fmt == 8:
            at = struct.unpack_from('>H', b, q)[0]; q += 2
            form = at & 15
            fake = {'attrs': {}}
            if form == 5: fake['attrs'][5] = struct.unpack_from('>H', b, q)[0]; q += 2
            elif form == 2: fake['attrs'][7] = struct.unpack_from('>I', b, q)[0]; q += 4
            elif form == 3:
                n = struct.unpack_from('>H', b, q)[0]; fake['attrs'][at >> 4] = b[q + 2:q + 2 + n]; q += 2 + n
            else: break
            elem = tname(fake, t['pos'], depth + 1)
        else: break
    return elem + ''.join(f'[{n}]' for n in dims)

def loc(a):
    b = a.get(2, b'')
    if len(b) >= 5 and b[0] in (3, 4): return int.from_bytes(b[1:5], 'big')
    return None

def children(r):
    """DIEs physically following r until its sibling offset."""
    i = bisect.bisect_left(POS, r['pos']) + 1
    end_off = r['attrs'][1]
    out = []
    while i < len(dies) and dies[i]['pos'] - r['pos'] < 65536:
        c = dies[i]
        if c['off'] >= end_off or c['off'] <= r['off']: break
        out.append(c); i = bisect.bisect_left(POS, c['pos'] + c['len'])
        if c['tag'] == T_STRUCT or c['tag'] == T_UNION:  # skip nested
            pass
    return out

def show_struct(s, label):
    a = s['attrs']
    print(f'struct {label}  size={a.get(11)}  @blob {s["pos"]}')
    for m in children(s):
        if m['tag'] != 0xd: continue
        o = loc(m['attrs'])
        print(f'   +{o if o is not None else "?":>4}  {tname(m, m["pos"]):28} {m["attrs"].get(3, "")}')

def find(name):
    hits = []
    for r in dies:
        if not isinstance(r['attrs'].get(3), str) or r['attrs'][3] != name: continue
        if r['tag'] in (T_STRUCT, T_UNION): hits.append((r, name))
        elif r['tag'] == T_TYPEDEF and 7 in r['attrs']:
            t = resolve(r['attrs'][7], r['pos'])
            if t and t['tag'] in (T_STRUCT, T_UNION): hits.append((t, name))
    return hits

mode = sys.argv[2]
if mode == 'struct':
    for name in sys.argv[3:]:
        seen = set()
        for s, label in find(name):
            sig = tuple((m['attrs'].get(3), loc(m['attrs'])) for m in children(s) if m['tag'] == 0xd)
            if not sig or sig in seen: continue
            seen.add(sig); show_struct(s, label)
        if not seen: print(f'struct {name}: not found / no members')
elif mode == 'globals':
    seen = set()
    for r in dies:
        if r['tag'] == 7 and isinstance(r['attrs'].get(3), str):
            k = (r['attrs'][3], loc(r['attrs']))
            if k in seen: continue
            seen.add(k)
            l = loc(r['attrs'])
            print(f'{l if l is not None else 0:08x}  {tname(r, r["pos"]):24} {r["attrs"][3]}')
elif mode == 'anon':   # anon <hex section offset> <blob pos near>
    s = resolve(int(sys.argv[3], 16), int(sys.argv[4]))
    show_struct(s, f'@{sys.argv[3]}')
