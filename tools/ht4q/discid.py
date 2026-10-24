"""Print a Dreamcast disc image's product number (IP.BIN), the game ID Flycast reports.

    python3 -I discid.py IMAGE      (.cue / .gdi / .cdi; prints nothing for .chd or an unreadable image)

Looks for the IP.BIN header ("SEGA SEGAKATANA") at the start of the image's data tracks; good enough to tell
our games apart (Hydro Thunder Rev 1: T-9702N, Crazy Taxi ReviveDC: RDC-0102, 18 Wheeler ReviveDC:
RDC-0094).
"""
import os, re, sys

MAGIC = b'SEGA SEGAKATANA '
CHUNK = 32 << 20


def scan(path, limit=None):
    """The product number in the first IP.BIN header in path (its first `limit` bytes), or None."""
    with open(path, 'rb') as f:
        tail = b''
        while True:
            block = f.read(CHUNK if limit is None else limit)
            if not block:
                return None
            data = tail + block
            i = data.find(MAGIC)
            if i >= 0 and i + 0x50 <= len(data):
                return data[i + 0x40:i + 0x4a].decode('ascii', 'replace').strip()
            if limit is not None:
                return None
            tail = data[-0x50:]


def track_files(image):
    d, lower = os.path.dirname(image), image.lower()
    if lower.endswith('.cue'):
        names = re.findall(r'^\s*FILE\s+"([^"]+)"', open(image, errors='replace').read(), re.M)
    elif lower.endswith('.gdi'):
        names = [m.group(1).strip('"') for m in (re.match(r'\s*\d+\s+\d+\s+\d+\s+\d+\s+("[^"]+"|\S+)', l)
                 for l in open(image, errors='replace').read().splitlines()[1:]) if m]
    elif lower.endswith('.cdi'):
        return [image]
    else:
        return []
    return [os.path.join(d, n) for n in names if os.path.exists(os.path.join(d, n))]


def disc_id(image):
    # .cue/.gdi: IP.BIN is the first sector of a data track (in track order); a .cdi has no index
    # we read, so it is searched (its first header is the disc's)
    whole = image.lower().endswith('.cdi')
    for f in track_files(image):
        found = scan(f, None if whole else 0x10000)
        if found:
            return found
    return None


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    found = disc_id(sys.argv[1])
    if found:
        print(found)
