# HydroThunder4Quest

Fork of Flycast (Dreamcast emulator) to play Dreamcast games on Meta Quest with stereo 3D and
head tracking: **Hydro Thunder** (done on the desktop and Quest) and **Crazy Taxi** (started
2026-10-24, branch `multigame`). Full plan: `docs/plan.md`.

Only these games are in scope — game-specific hacks are fine, kept in a folder per game:
`core/ht4q/<game>/` behind the game table in `core/ht4q/game.h` (keyed by the disc's game ID).
Everything else in `core/ht4q`, the stereo renderer and the XR presenter is shared.

## Layout

Everything lives under one project folder; `flycast/` is the repo we work in:

```
HydroThunder4Quest/
  flycast/     this repo: Flycast fork (`origin` = Daniel-Oberlin/flycast, `upstream` = flyinghead/flycast)
  gamefiles/   disc images (never commit)
  notes/       per game: hydrothunder/ (recovered Midway source notes), crazytaxi/ (never commit)
  textures/    per game: hydrothunder/, crazytaxi/ - dumped / AI-upscaled textures (game-derived,
               never commit here; each its own local-only git repo with no remote and a pre-push
               hook that refuses - never add a remote, never mix games)
  tools/       third-party tools (Real-ESRGAN binary, Stable Diffusion env)
```

AI models are not stored here: they live in the shared store `~/Desktop/dev/ai-models`
(see its README; `source ~/Desktop/dev/ai-models/env.sh` sets `HF_HOME`).

- Game files (outside repo): `../gamefiles/`
  - Hydro Thunder: **Hydro Thunder (USA) (Rev 1)** bin/cue, game ID T-9702N. Its RAM addresses
    (`core/ht4q/hydrothunder/`) are for this build only. (A `Hydro Thunder (USA).cdi` seen earlier
    was a CD-R conversion of an older build with a ChuChu Rocket header - never use one for address work.)
  - Crazy Taxi: `Crazy Taxi (USA).cdi`, the ReviveDC CD-R rebuild - game ID **RDC-0102**, not the
    retail MK-51035 (IP.BIN V1.004 dated 2012-07-07, game code dated 1999). Its addresses
    (`core/ht4q/crazytaxi/`) are for this image; a retail GD-ROM dump would need checking.
  - `tools/ht4q/discid.py IMAGE` prints an image's game ID.
- Notes (outside repo, never commit): `../notes/hydrothunder/` (recovered Midway source),
  `../notes/crazytaxi/`
- Texture upscaling: `tools/ht4q/hqtex.py` (the shareable end-to-end tool: players rebuild the pack from
  their own game, textures are never distributed; `--game-id` per game) around `tools/ht4q/texup.py`;
  our inputs and outputs in `../textures/<game>/`.

## Rules

- Never commit or download game images, BIOS files, or extracted Midway source.
- Fork changes stay GPL-compatible.
- Keep changes to Flycast core small and isolated (prefer new files + minimal
  call-site hooks) so they rebase onto upstream easily.
- Develop and prove everything on macOS first; Quest comes last.

## Build (macOS, arm64)

Upstream's interactive path is `shell/apple/generate_xcode_project.command`.
Non-interactive equivalent used here — see "Build notes" below.

### Build notes

```sh
# configure (once; OpenGL only, Vulkan/MoltenVK off for now)
cmake -B build -G Xcode -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DUSE_VULKAN=OFF -DUSE_BREAKPAD=NO -DCMAKE_XCODE_GENERATE_SCHEME=YES
# build
cmake --build build --config RelWithDebInfo --target flycast -- -quiet -jobs $(sysctl -n hw.ncpu)
# run (no BIOS needed: boots via reios HLE; game ID T-9702N)
build/RelWithDebInfo/Flycast.app/Contents/MacOS/Flycast \
  "../gamefiles/Hydro Thunder (USA) (Rev 1)/Hydro Thunder (USA) (Rev 1).cue"
```

`-config section:key=value` options must come before the game path (Flycast ignores everything
after it). `tools/ht4q/run.sh` does this (memserver on, muted; `HT4Q_GAME=ct` for Crazy Taxi).

Flycast config/data: `~/Library/Application Support/Flycast/`.

Phase 1 verified (2026-10-20): runs smooth, all 3 camera views OK, no glitches.
The game renders at **30 fps natively** (Flycast's `F:` counter = game frames; the game
waits 2 fields/frame via `Gameloop_nTargetFieldsPerFrame`). Implications: compositor
reprojection on Quest is essential; patching that value toward 60 fps is a possible
later experiment.

## Recovered game structures (Phase 2)

Notes: `../notes/hydrothunder/phase2-structures.md` (struct layouts, gamecam statics,
draw path). Recovery tools: `tools/ht4q/`. Debug-build addresses there are NOT retail — use
them as layout/order signatures, confirm in retail RAM.

## Camera hook (Phase 4)

Verified 2026-10-21: moving the mouse over the window sways/turns the in-game camera in real
time. `core/ht4q/hydrothunder/camhook.cpp` auto-installs for T-9702N; head pose is OpenXR-style (x right,
y up, z back; +yaw left, +pitch up). Options and tools: `tools/ht4q/README.md`.

## Quest (Phase 6)

`quest` Gradle flavor (`com.flycast.emulator.ht4q`), helper `tools/ht4q/quest.sh build|install|push|run|shot|log`.
Renders into a 1440x1080 pbuffer and presents it as an OpenXR quad (`.../jni/src/ht4q_xr.cpp`).
Game files go to app-private storage via `run-as` (debug builds only). Memserver is always on:
`adb forward tcp:3264 tcp:3264`, then the desktop tools (drive.py etc.) work unchanged.
Headset not worn = asleep: `adb shell am broadcast -a com.oculus.vrpowermanager.prox_close`
(undo with `...automation_disable`).
