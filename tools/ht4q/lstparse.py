"""Parse compiler-listing fragments out of a raw data blob.

Record:  [binary prefix] <name, right-aligned in 16> <lineno in 6> ' ' <flag> '  ' <text>
Continuation:  25 spaces, '+', <text>  (appended directly to the previous line)
Groups consecutive records of one file into runs; pickles them for later stages.
"""
import re, sys, pickle, hashlib
from collections import defaultdict

REC = re.compile(rb'([!-~]{1,16}) {1,6}(\d{1,6}) ([ -~])  ([^\n]*)$')
CONT = re.compile(rb' {25}\+([^\n]*)$')

def runs(blob):
    cur = None              # (name, start_offset, {line: text}, last_line)
    pos = 0
    for raw in blob.split(b'\n'):
        off = pos
        pos += len(raw) + 1
        m = CONT.search(raw)
        if m and cur:
            cur[2][cur[3]] += m.group(1).decode('latin1')
            continue
        m = REC.search(raw)
        if not m:
            if cur and raw.strip(b'\x00 ') == b'':
                continue
            if cur: yield cur
            cur = None
            continue
        # name field must occupy exactly 16 chars ending where the number field begins
        name_end = m.end(1)
        num_end = m.end(2)
        if num_end - name_end > 6 or len(raw[:num_end]) < 22:
            if cur: yield cur
            cur = None
            continue
        name = m.group(1).decode('latin1')
        line = int(m.group(2))
        text = m.group(4).decode('latin1')
        if cur and cur[0] == name and line > cur[3]:
            cur[2][line] = text
            cur = (cur[0], cur[1], cur[2], line)
        else:
            if cur: yield cur
            cur = (name, off, {line: text}, line)
    if cur: yield cur

blob = open(sys.argv[1], 'rb').read()
# Record prefix is u16 file id, u16 line, u16 0: a 0x0a byte in it (line 10, 266, ...)
# would split the record, so neutralize newlines inside prefixes before splitting.
PREFIX = re.compile(rb'\n([\s\S]{4})\x00\x00(?= {0,15}[!-~]{1,16} {1,6}\d| {25}\+)')
blob = PREFIX.sub(lambda m: b'\n' + m.group(1).replace(b'\n', b'\x01') + b'\x00\x00', blob)
out = defaultdict(list)
for name, off, lines, _ in runs(blob):
    if len(lines) < 3:
        continue
    h = hashlib.sha1('\n'.join(f'{k}:{v}' for k, v in sorted(lines.items())).encode()).hexdigest()[:10]
    out[name].append(dict(off=off, lines=lines, hash=h))
pickle.dump(dict(out), open(sys.argv[2], 'wb'))
print(len(out), 'distinct names;', sum(len(v) for v in out.values()), 'runs')
