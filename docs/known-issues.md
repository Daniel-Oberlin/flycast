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
- **Next:** make the game build the sky for the head direction - find the race-mode
  `sky_Work` call (not in the recovered listings, which only cover attract mode) and add the
  head offset to its arguments, like the BuildCam stub.

### Sun visible inside tunnels (logged 2026-10-21, milestone 3)
- Not yet checked whether the unmodified game does the same (compare on desktop without hook).

### Sun appears at the wrong depth in stereo (logged 2026-10-21, Quest milestone 2)
- **Seen:** in the headset (3D virtual screen), the sun doesn't sit at the far distance.
- **Likely cause (unverified):** the sun / lens-flare sprites are drawn with a large 1/w, so the
  stereo renderer treats them as HUD (1/w >= `HT4Q_STEREO_HUDZ`, 0.75) and pins them to the HUD
  depth instead of infinity. Check with a frame dump (`drive.py dump:NAME`) near the sun.
- **Fix idea:** identify the sun/flare polygons (texture or list) and force them to 1/w = 0.
- **Update (milestone 3):** looks right in the immersive VR view; still to check on the 3D screen.
