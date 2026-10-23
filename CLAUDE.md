# HydroThunder4Quest

Fork of Flycast (Dreamcast emulator) to play **Hydro Thunder (Dreamcast)** on Meta
Quest with stereo 3D and head tracking. Full plan: `docs/plan.md`.

Only this one game is in scope — game-specific hacks are fine.

## Layout

Everything lives under one project folder; only `flycast/` is the git repo:

```
HydroThunder4Quest/
  flycast/     this repo: Flycast fork (`origin` = Daniel-Oberlin/flycast, `upstream` = flyinghead/flycast)
  gamefiles/   disc images (never commit)
  notes/       recovered Midway source notes (never commit)
  textures/    dumped / AI-upscaled textures (game-derived, never commit)
  tools/       third-party tools (Real-ESRGAN binary, Stable Diffusion env)
```

AI models are not stored here: they live in the shared store `~/Desktop/dev/ai-models`
(see its README; `source ~/Desktop/dev/ai-models/env.sh` sets `HF_HOME`).

- Game files (outside repo): `../gamefiles/`
  - Target build: **Hydro Thunder (USA) (Rev 1)** bin/cue. All RAM addresses we
    record are for this build only.
  - Also present: `Hydro Thunder (USA).cdi` (revision unverified — don't use for address work).
- Notes on recovered Midway source (outside repo, never commit): `../notes/`
- Texture upscaling: `tools/ht4q/texup.py`; inputs and outputs in `../textures/`.

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

Flycast config/data: `~/Library/Application Support/Flycast/`.

Phase 1 verified (2026-10-20): runs smooth, all 3 camera views OK, no glitches.
The game renders at **30 fps natively** (Flycast's `F:` counter = game frames; the game
waits 2 fields/frame via `Gameloop_nTargetFieldsPerFrame`). Implications: compositor
reprojection on Quest is essential; patching that value toward 60 fps is a possible
later experiment.

## Recovered game structures (Phase 2)

Notes: `../notes/phase2-structures.md` (struct layouts, gamecam statics,
draw path). Recovery tools: `tools/ht4q/`. Debug-build addresses there are NOT retail — use
them as layout/order signatures, confirm in retail RAM.

## Camera hook (Phase 4)

Verified 2026-10-21: moving the mouse over the window sways/turns the in-game camera in real
time. `core/ht4q/camhook.cpp` auto-installs for T-9702N; head pose is OpenXR-style (x right,
y up, z back; +yaw left, +pitch up). Options and tools: `tools/ht4q/README.md`.

## Quest (Phase 6)

`quest` Gradle flavor (`com.flycast.emulator.ht4q`), helper `tools/ht4q/quest.sh build|install|push|run|shot|log`.
Renders into a 1440x1080 pbuffer and presents it as an OpenXR quad (`.../jni/src/ht4q_xr.cpp`).
Game files go to app-private storage via `run-as` (debug builds only). Memserver is always on:
`adb forward tcp:3264 tcp:3264`, then the desktop tools (drive.py etc.) work unchanged.
Headset not worn = asleep: `adb shell am broadcast -a com.oculus.vrpowermanager.prox_close`
(undo with `...automation_disable`).
