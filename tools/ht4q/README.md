# ht4q recovery tools

Run against your own disc dump; outputs go to `../notes/` (outside the repo,
never commit them). Run Python with `-I`.

1. `bin2user.py <track37.bin> <out.user.bin>` — strip raw 2352-byte sectors to Mode-1 user data.
2. `lstparse.py <user.bin> runs.pkl` — find SHC compiler-listing fragments (source with line numbers).
3. `merge.py runs.pkl src/` — rebuild one file per header; marks gaps/conflicts.
4. `dwarf1scan.py <user.bin> dwarf.pkl` — scan for big-endian DWARF v1 DIEs.
5. `dwarf1dump.py dwarf.pkl struct <Name>...` / `globals` / `anon <hexoff> <blobpos>` — layouts and addresses.

## Live tools (Phase 3+)

Launch with `tools/ht4q/run.sh` (sets `HT4Q_MEMSERVER=3264`; see `core/ht4q/memserver.cpp`).

- `dcmem.py` — client: read/write main RAM, virtual controller (`input()` / `release()`), `pc()` = the
  SH4's current pc (sample it repeatedly to see where the game spends its time).
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
- `HT4Q_HUD_LOCK=1` — keep the HUD still in the unhooked camera's frame (where the boat is heading)
  while the head turns, so you can look over at it. Default on in the headset, off on the desktop.
- `HT4Q_FRAMEDUMP=$SHOTDIR/frame.txt` — enables `drive.py shot:NAME` (rendered frame from
  Flycast, never the screen) and `dump:NAME` (polygon lists with 1/w ranges).

## 60 fps (`core/ht4q/framerate.cpp`, option `ht4q.FrameRate60`, Settings > Display)

The game runs at 30 because Kamui shows a frame every 2nd vblank (divider `0x8c3cf23c` = 1) and its
logic steps a fixed 1/30 s per frame (motion integrator literal `0x8c05d8c0`; timers use the Gameloop
timing block at `0x8c1fdab4`). The option sets the divider to 0, the timing block to 60 fps and the
integrator step to 1/60. Desktop: steady 60 fps, boat distance vs real time within 3% of 30 fps.
Desktop test: `-config ht4q:FrameRate60=yes` before the game path. Not yet measured on the Quest.

## 2-player full screen (`core/ht4q/twoplayer.cpp`, option `ht4q.TwoPlayerFull`; branch `twoplayer`)

The game's 2-player mode (main menu RIGHT; player 2 joins at the boat select) splits the screen
top/bottom: its gameloop (`0x8c073710`) draws each player after calling the viewport setter
`0x8c098394(height, top)` - player 1 (256, 0), player 2 (224, 256) - which writes
`dcViewportYTrans/H/YMin/YMax` (`0x8c1b05b8`) and sets the PVR user tile clip to those rows. The views
are crops of a normal 640x480 projection. The option patches four literals so both players get a full
640x480 view, player 2's 480 px below the screen (tile rows 15-29); the renderer draws such frames once
per player, moved up 480 px for player 2 (`TransformMatrix::ht4qShiftY`, which also keeps tile clips
covering the whole 640x480 frame in force). `HT4Q_2P_VIEW=1|2|both` picks what is shown (both = side by
side, desktop without stereo; default both on the desktop, 1 in the headset).
Desktop test: `-config input:device2=0 -config ht4q:TwoPlayerFull=yes` before the game path, then
`drive.py to2p`. Other Rev 1 addresses: `Player_nHumanCount` `0x8c25978c`, `Player_nHuman` (player being
processed) `0x8c25977c`, `bEUROCOM_SPLITSCREEN` `0x8c1b05e0` (the Eurocom flags block from `0x8c1b05cc`:
world-object work and collision scans only for objects in view are on).

## Launcher and controls (`core/ht4q/launcher.cpp`, `core/ht4q/controls.cpp`)

Startup screen before the game boots: Start game, or Settings (tabs: Buttons, Steering, Display,
Rumble). Point with either controller (aim ray on the screen quad = mouse, trigger = click); menus
open 2 m in front of the head. The logo is the title-screen texture `5069ed0e.png`, read from the
player's own HQ pack or texture dump (game artwork, never shipped); without it the title is text.
Quit ends the app; on Quest the process always ends with its activity (a recreated activity can't
get the OpenXR session back). While adjusting the view in a race, a panel with the HUD size and
depth effect values is drawn by the OSD (`ADJUST_PANEL` in `ht4q.h`) and shown head-locked by the
XR presenter. Test hook: write `main`, a tab number, `start` or `adjust` to
`<ext files>/ht4q_ui.req` (adb push), then `quest.sh shot` (while adjusting it captures the panel). Settings are saved to `emu.cfg` (`[ht4q]` section, plus `rend.CustomTextures`):

- Button mapping: two controller inputs per function (throttle, brake, boost, view, DC B/X, start).
- Steering: right stick, left stick, or tilting either controller left/right; optional throttle/brake
  from tilting forward/back (with a "no-throttle position" calibration and a tilt range).
- Textures: original or HQ (only if `textures/T-9702N` is installed).
- Haptics: the game's Puru Puru (Jump Pack) rumble -> both controllers, with a strength. Starting the
  game puts a Puru Puru pack in controller slot 2.

- View adjustment, in a race (all remappable): right stick press pauses (the game's own pause, its
  box hidden) and the sticks then change the HUD size (right, fwd/back) and the depth effect (left,
  fwd/back: eye separation, % of real); right stick press again resumes and saves. Left stick press
  resets both, any time. The game's pause flag is `0x8c259a08` (1 = paused); Start toggles it.

Without a content path (started from the Quest's app library) it boots the disc image in the app's
private `files/game/` (Rev 1 .cue preferred).

`HT4Q_LAUNCHER=0` skips it in the headset; `HT4Q_LAUNCHER=1` shows it on the desktop (mouse).

## Installing on someone's Quest (`install.sh`)

`tools/ht4q/install.sh` installs the app and the player's own disc image: it searches `--roms DIR`
(repeatable), `$HT4Q_ROMS` (colon-separated), then `../gamefiles`, `~/ROMs`, `~/roms`, `~/Games`,
`~/Downloads`, `~/Desktop`, `~/Documents` for a Hydro Thunder .cue/.gdi/.chd/.cdi (Rev 1 .cue preferred),
or takes `--game FILE`. Copies the image and its track files into the app's private storage
(through `run-as`, so the debug APK), optionally HQ textures (`--textures DIR`), and starts it.
Builds the APK first if there is none (`--apk FILE` to use a given one).

## HQ textures (`hqtex.py`, shareable)

The upscaled textures come from the game, so they are never distributed: each player builds their
own pack with `tools/ht4q/hqtex.py` - `setup` (downloads Real-ESRGAN ncnn-vulkan, checksum-checked
on macOS), `dump-quest on|off` + `pull-quest DIR` (collect the textures the game loads while
playing, on the Quest), `dump-desktop DIR [--tracks thunder,lost,arctic]` (automatic, with this
fork's desktop Flycast: per track it steers the menus, starts a race, moves the camera onto an AI boat
(`ATTACHED_PHYS` -> its physics) so the whole course is seen, keeps the player's checkpoint timer
(`0x8c2597bc`) full, and quits at the end or after `--minutes`; Rev 1 addresses; about 5 min per
track), `upscale DUMP HQ [--scale 2|4]` (runs `texup.py`, which skips the minimap - redrawn into a
new texture every frame - and textures already in HQ), `install-quest HQ` / `install-desktop HQ`.
Run it without arguments for the full steps.
`texup.py` is the upscaling itself (per-axis wrap padding, alpha handling); `texcompare.py` builds a
before/after gallery for development.
