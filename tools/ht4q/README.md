# ht4q recovery tools

Run against your own disc dump; outputs go to `../HydroThunder4Quest-notes/` (outside the repo,
never commit them). Run Python with `-I`.

1. `bin2user.py <track37.bin> <out.user.bin>` — strip raw 2352-byte sectors to Mode-1 user data.
2. `lstparse.py <user.bin> runs.pkl` — find SHC compiler-listing fragments (source with line numbers).
3. `merge.py runs.pkl src/` — rebuild one file per header; marks gaps/conflicts.
4. `dwarf1scan.py <user.bin> dwarf.pkl` — scan for big-endian DWARF v1 DIEs.
5. `dwarf1dump.py dwarf.pkl struct <Name>...` / `globals` / `anon <hexoff> <blobpos>` — layouts and addresses.
