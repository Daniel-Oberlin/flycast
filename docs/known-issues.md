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
- **Fix (2026-10-21, awaiting headset check):** the sky panels are built by
  `sky_Work(heading, pitch, roll)` at 0x8c0744ee, called per frame by the camera update
  (0x8c046302) with the *unhooked* draw angles - so the sky was glued to the view. Its seven
  call literals now go through a stub (`camhook.cpp`, SKYWORK_STUB) that adds the head offsets,
  like the BuildCam stub, so the game builds the sky for the rendered view. The game lays the
  sky out by angle at a fixed ~270 px per radian (independent of the FOV), so in VR the vertex
  shader re-projects sky vertices with the real projection (`HT4Q_SKY_CORRECT` = px/radian,
  `stereo.py sky on|off|N`). A second, minor sky element reads the boat heading through
  0x8c04b890 / 0x8c04c07c / 0x8c04c424; those point at the camera heading too.
  Found with temporary read/write traces in `addrspace::readt/writet` under the interpreter
  (`-config config:Dynarec.Enabled=no`).
- **Limits:** each sky panel is ~60 deg wide with only 4 vertices, so after re-projection its
  interior is off by a few degrees near the edges of the view; the game also clamps the top of
  the sky to the top of the screen when looking up.

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
