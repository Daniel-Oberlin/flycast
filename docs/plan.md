# Hydro Thunder 4 Quest — Project Plan

Play the Dreamcast version of Hydro Thunder on Meta Quest with stereo 3D and
head tracking (at least side-to-side), using a fork of the Flycast emulator.

Only one game is in scope, so game-specific hacks are acceptable and expected.

---

## Approach at a glance

| Problem | Approach |
|---|---|
| Emulator | Fork of Flycast (open source, GPL; already has an Android build) |
| Camera view | The game's first-person view (Y button cycles 2 chase cams + first-person) |
| Head tracking | Find the game camera in emulated RAM; add the headset offset each frame before the game transforms geometry |
| Stereo | Per-vertex disparity shift in Flycast's renderer using each vertex's 1/w |
| VR runtime | OpenXR on Quest, per-eye swapchains, multiview rendering |
| Latency | Submit depth to the compositor so it can reproject |

Develop and prove everything on the Mac first. The Quest comes last.

---

## Phase 0 — Setup

**Tasks**
- Fork `flyinghead/flycast` on GitHub and clone it with submodules into this folder.
- Add `CLAUDE.md` (project brief) to the repo root.
- Create a folder outside the repo for the user's own disc dump and BIOS files;
  add their paths to `.gitignore` anyway.
- Install Claude Code and start sessions from the repo root.

**Done when:** the fork is cloned, the brief is in place, game files are stored
outside the repo.

---

## Phase 1 — Build and run on macOS

**Tasks**
- Build Flycast from source following the repo's own macOS build docs.
- Run Hydro Thunder from the user's dump; confirm all three camera views work.
- Note which disc build is being used (USA, Rev 1 or earlier) — addresses found
  later are specific to it.

**Done when:** a self-built Flycast runs the game at full speed on the Mac.

---

## Phase 2 — Recover structures from the leftover source

**Tasks**
- Extract readable strings from track 37 of the user's own retail dump.
- Reassemble the fragments of `worldob.h`, `xmath.h`, `glide.h`, `sky.h`,
  `material.h` into notes kept outside the repo (proprietary Midway code — do not
  commit or redistribute).
- Write down: the world-object struct layout, the matrix/vector format, and the
  Glide-style draw entry points.

**Done when:** we know what a world object and a camera matrix should look like
in RAM.

**Reference:** tcrf.net, "Hydro Thunder (Dreamcast)/Uncompiled Source Code".
Prototype builds carry more code but may not match retail.

---

## Phase 3 — Find the camera

**Tasks**
- Add (or use existing) Flycast tooling for memory search: snapshot RAM, filter for
  floats that change when the camera moves and hold still when paused.
- Narrow down to the camera position and orientation (likely a matrix inside a
  world object).
- Use the SH-4 debugger to find the code that reads the camera and builds the
  view transform each frame.
- Prove control: write an offset into the camera and watch the view move.

**Done when:** we can move the first-person view sideways by poking memory, with
correct parallax and no visual breakage.

---

## Phase 4 — Camera injection hook

**Tasks**
- Add a small, isolated hook in the Flycast fork that applies a live offset
  (translation + yaw) to the camera at the right point in each frame.
- Drive it from mouse or keyboard as a stand-in for the headset.
- Optional: an option to level the horizon (cancel the boat's pitch/roll on the
  camera) to reduce motion sickness later.

**Done when:** moving the mouse sways the in-game viewpoint smoothly in real time.

---

## Phase 5 — Stereo on desktop

**Tasks**
- In Flycast's vertex shader, render the frame twice with a per-eye shift:
  - Left eye: `x − k · (1/w − 1/c)`
  - Right eye: `x + k · (1/w − 1/c)`
  - `k` = stereo strength, `c` = convergence depth.
- Output side-by-side (or anaglyph for quick checking) on the Mac.
- Pin the HUD (boost meter, timer, position) to a fixed depth by overriding its 1/w.
- Tune `k` and `c` for this game; cap far-distance separation so eyes never diverge.

**Done when:** the game looks correctly three-dimensional in side-by-side view,
with a readable HUD.

---

## Phase 6 — Port to Quest

**Tasks**
- Start from Flycast's Android build; get it running on the Quest as-is.
- Integrate OpenXR: session, per-eye swapchains, multiview.
- Feed the headset pose into the Phase 4 camera hook.
- Feed the Phase 5 stereo shift into the per-eye render.
- Submit depth with each frame so the compositor can reproject.
- Map controls to Quest controllers (or a paired gamepad).

**Done when:** the game runs on the Quest in stereo with side-to-side head tracking.

---

## Phase 7 — Comfort and polish

**Tasks**
- Measure and reduce pose-to-photon latency.
- Widen or patch frustum culling if head movement exposes missing geometry.
- Tune internal resolution against Quest performance.
- Settings menu for stereo strength, convergence, horizon leveling.

---

## Risks

| Risk | Mitigation |
|---|---|
| Latency through the game's frame pipeline causes VR sickness | Depth submission + compositor reprojection; inject pose as late as possible |
| Frustum culling hides geometry when the head moves | Keep offsets modest; patch the game's culling values |
| First-person camera pitches/rolls with the boat | Optional horizon leveling |
| HUD and sprites at odd depths | Per-element 1/w overrides |
| Addresses differ between disc revisions | Lock to one build and record it |
| Quest performance with two eye renders | Multiview, modest internal resolution |

---

## Rules

- Never commit or download game images, BIOS files, or extracted Midway source.
- Fork changes stay GPL-compatible.
- Keep changes to Flycast core small and isolated so they rebase onto upstream easily.
