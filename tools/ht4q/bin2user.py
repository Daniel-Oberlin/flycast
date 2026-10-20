"""Strip raw 2352-byte CD sectors to their 2048-byte Mode 1 user data."""
import sys
SYNC = bytes([0]) + bytes([0xFF]) * 10 + bytes([0])
src, dst = sys.argv[1], sys.argv[2]
counts = {}
with open(src, 'rb') as f, open(dst, 'wb') as out:
    while (sec := f.read(2352)):
        if sec[:12] != SYNC:
            counts['nosync'] = counts.get('nosync', 0) + 1
            continue
        mode = sec[15]
        counts[mode] = counts.get(mode, 0) + 1
        if mode == 1:
            out.write(sec[16:16 + 2048])
print(counts)
