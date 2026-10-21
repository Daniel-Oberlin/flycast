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
- **Fix (2026-10-21, awaiting headset check):** the sky builder (0x8c04c314, plus helpers
  0x8c04b6f0 / 0x8c04bf24) reads the heading through three literals (0x8c04b890, 0x8c04c07c,
  0x8c04c424 -> 0x8c256570). `camhook.cpp` points them at `skyHeading`, which the BuildCam stub
  fills with the heading the camera is actually built with (boat + head yaw), so the game
  generates the cloud sky for wherever you look. `HT4Q_SKYHOOK=0` turns it off.
  The earlier "redirect had no effect" result was wrong - runtime literal patches do work.
  Found with a temporary write trace in `addrspace::writet` under the interpreter
  (`-config config:Dynarec.Enabled=no`): writers of the sky vertex block 0x8c20077c.
- **Still open:** pitch (the horizon line, 0x8c200a3c) isn't driven by the head; and on some
  tracks the distant mountains may be part of the sky layer rather than scenery - to check.

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
