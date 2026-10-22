# Known issues

## Open

### Sky / distant mountains don't follow head rotation in VR (logged 2026-10-21, milestone 3)
- **Seen:** in the immersive race view the mountain/cloud backdrop turns with the head (stays
  put on screen like the HUD) instead of staying fixed in the world.
- **Cause:** the game draws the sky as screen-space quads (1/w 1e-5) placed by
  `sky_Work(Phys.nHeading, Phys.nPitch, Phys.nRoll)` - the boat's physics angles
  (`Player_aData[n].Phys`, heading at `0x8c256570` = `_pAttachedPhys` + 0x68), not the camera.
  It also scrolls at about half the rate of real geometry when the boat turns.
- **Tried:** re-projecting the sky quads in the vertex shader by the head rotation
  (`skyCorrect`, commit 7a35efc73). Correct, but the game only generates sky for its forward
  view, so in the headset you see a world-fixed "window" of sky and black elsewhere. Off by
  default; `stereo.py sky on` to compare.
- **Fix, round 2 (2026-10-21):** the sky panels are built by `sky_Work(heading, pitch, roll)`
  at 0x8c0744ee, called per frame by the camera update (0x8c046302) with the *unhooked* draw
  angles - so the sky was glued to the view. Its seven call literals now go through
  SKYWORK_STUB (`camhook.cpp`) with its own SKY_POSE block. In the headset it passes heading +
  head yaw with pitch/roll zeroed (a level panorama); the renderer (`ht4q_stereo.cpp`) draws
  the sky polygons into an off-screen capture and paints each eye's background per pixel
  (direction -> azimuth/elevation -> the game's layout, ~270 px/radian, horizon y read from the
  sky vertices), hiding the sky polygons in the eye passes. Desktop test: `HT4Q_SKY_TEST=1
  HT4Q_LEVEL=all`; frame-pair rotation fit gives exactly 1.0x for yaw and pitch.
  Found with temporary read/write traces in `addrspace::readt/writet` under the interpreter
  (`-config config:Dynarec.Enabled=no`).
- **Headset feedback on round 1** (vertex re-projection): much better, slight drift when looking
  up and sideways - the reason for round 2. Round 2 not yet tried in the headset.
- **Verified in the headset (2026-10-22):** round 2 holds still under head motion, and the
  black sky seen partway through two tracks with round 1 is gone.
  Looking far up repeats the sky's top row (the game draws nothing above ~59 deg).

### Sun visible inside tunnels (logged 2026-10-21, milestone 3)
- Not yet checked whether the unmodified game does the same (compare on desktop without hook).

### Sun appears at the wrong depth in stereo (logged 2026-10-21, Quest milestone 2)
- **Seen:** in the headset (3D virtual screen), the sun doesn't sit at the far distance.
- **Likely cause (unverified):** the sun / lens-flare sprites are drawn with a large 1/w, so the
  stereo renderer treats them as HUD (1/w >= `HT4Q_STEREO_HUDZ`, 0.75) and pins them to the HUD
  depth instead of infinity. Check with a frame dump (`drive.py dump:NAME`) near the sun.
- **Fix idea:** identify the sun/flare polygons (texture or list) and force them to 1/w = 0.
- **Update (milestone 3):** looks right in the immersive VR view; still to check on the 3D screen.

### Water has no depth for the compositor (logged 2026-10-21, Phase 7 depth submission)
- **Seen:** reading back the per-eye depth sent to the headset, parts of the water surface read
  as infinitely far (cleared depth) while scenery, the boat and the HUD read correctly.
- **Cause:** the water is drawn as translucent polygons without depth writes, so Flycast's depth
  buffer keeps whatever is behind it (nothing, in open water).
- **Effect:** the compositor reprojects those pixels as if far away - the same as before depth
  was submitted. Possible small stretching where near opaque objects meet open water.
- **Fix idea, if it shows:** fill cleared pixels in the lower half with the water plane's depth,
  or let translucent passes write depth into our copy only.
