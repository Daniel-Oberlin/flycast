# HydroThunder4Quest

Fork of Flycast (Dreamcast emulator) to play **Hydro Thunder (Dreamcast)** on Meta
Quest with stereo 3D and head tracking. Full plan: `docs/plan.md`.

Only this one game is in scope — game-specific hacks are fine.

## Layout

- Repo root = Flycast fork (`origin` = Daniel-Oberlin/flycast, `upstream` = flyinghead/flycast).
- Game files (outside repo): `../HydroThunder4Quest-gamefiles/`
  - Target build: **Hydro Thunder (USA) (Rev 1)** bin/cue. All RAM addresses we
    record are for this build only.
  - Also present: `Hydro Thunder (USA).cdi` (revision unverified — don't use for address work).
- Notes on recovered Midway source (outside repo, never commit): `../HydroThunder4Quest-notes/`

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
  "../HydroThunder4Quest-gamefiles/Hydro Thunder (USA) (Rev 1)/Hydro Thunder (USA) (Rev 1).cue"
```

Flycast config/data: `~/Library/Application Support/Flycast/`.

## Recovered game structures (Phase 2)

Notes: `../HydroThunder4Quest-notes/phase2-structures.md` (struct layouts, gamecam statics,
draw path). Recovery tools: `tools/ht4q/`. Debug-build addresses there are NOT retail — use
them as layout/order signatures, confirm in retail RAM.
