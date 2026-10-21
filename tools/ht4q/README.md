# ht4q recovery tools

Run against your own disc dump; outputs go to `../HydroThunder4Quest-notes/` (outside the repo,
never commit them). Run Python with `-I`.

1. `bin2user.py <track37.bin> <out.user.bin>` — strip raw 2352-byte sectors to Mode-1 user data.
2. `lstparse.py <user.bin> runs.pkl` — find SHC compiler-listing fragments (source with line numbers).
3. `merge.py runs.pkl src/` — rebuild one file per header; marks gaps/conflicts.
4. `dwarf1scan.py <user.bin> dwarf.pkl` — scan for big-endian DWARF v1 DIEs.
5. `dwarf1dump.py dwarf.pkl struct <Name>...` / `globals` / `anon <hexoff> <blobpos>` — layouts and addresses.

## Live tools (Phase 3+)

Launch with `tools/ht4q/run.sh` (sets `HT4Q_MEMSERVER=3264`; see `core/ht4q/memserver.cpp`).

- `dcmem.py` — client: read/write main RAM, virtual controller (`input()` / `release()`).
- `drive.py` — scripted play: `torace`, `gason`, `view:0`, `tap:A`, `eye:...`, `poke:ADDR:F`, `shot:NAME`, `cam`,
  `head:X:Y:Z:YAW:PITCH:ROLL` / `head:off`, `hook`, `campos`.
  Example: `python3 -I tools/ht4q/drive.py torace gason view:0 wait:1 cam shot:fp gasoff`
- `camfind.py` — find the gamecam statics by signature. `camlocal.py`, `fplog.py`, `campoke.py`,
  `eyeshift.py` — experiments from the camera search.
- `shot.sh` — full-screen screenshot, downscaled.
- `sh4dis.py ADDR [COUNT]` — SH-4 disassembler (Flycast's opcode table) over live RAM;
  `xref.py` — functions that load given addresses from their literal pools.
- `camhook.py status | head X Y Z YAW PITCH ROLL | mouse` — the camera head-pose hook.

## Camera head-pose hook (Phase 4, `core/ht4q/camhook.cpp`)

Flycast installs it by itself for game ID T-9702N once the gamecam code is in RAM. Head pose
(right-handed like OpenXR: x right, y up, z back, +yaw left, +pitch up, +roll tilt left) comes
from the mouse over the game window, or from the memserver `'H'` override (`drive.py head:...`).

- `HT4Q_MOUSEHEAD=both|look|lean|off` — mouse drives yaw/pitch (±40°/±25°) and/or lean (±2/±1 units). Default both on desktop, off in Quest builds; inactive until the mouse first moves.
- `HT4Q_LEVEL=roll|all` — cancel the game camera's roll (and pitch) to keep the horizon level.
- `HT4Q_CAMHOOK=0` — don't install.

## Stereo (Phase 5, `core/rend/gles/ht4q_stereo.cpp`)

Each eye is the same frame with x shifted by `-eye * sep * (conv/w - 1)` in the vertex shader
(uniform `ht4q_stereo`). HUD vertices (1/w >= HUDZ; the game draws its HUD at 1/w = 1 and 10)
are shifted as if at depth HUD.

- `HT4Q_STEREO=sbs|anaglyph` — half side-by-side (left eye on the left) or red/cyan.
- `HT4Q_STEREO_SEP=0.015` — per-eye shift at infinity, NDC (screen width = 2).
- `HT4Q_STEREO_CONV=20` — convergence depth, game units (boat in chase view is ~18-25 away).
- `HT4Q_STEREO_HUDZ=0.75`, `HT4Q_STEREO_HUD=15` — HUD detection threshold and pinned depth.
- `HT4Q_FRAMEDUMP=$SHOTDIR/frame.txt` — enables `drive.py shot:NAME` (rendered frame from
  Flycast, never the screen) and `dump:NAME` (polygon lists with 1/w ranges).
