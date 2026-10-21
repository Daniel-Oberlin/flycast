# Known issues

## Open

### Sky / distant mountains don't follow head rotation in VR (logged 2026-10-21, milestone 3)
- **Seen:** in the immersive race view the mountain/cloud backdrop turns with the head (stays
  put on screen like the HUD) instead of staying fixed in the world.
- **Verified:** on desktop, a 15 deg hooked yaw shifts the sky quads (1/w = 1e-5, 3 quads) by ~2-13 px
  instead of ~86 px. The camera matrices (`_CamXfmData`) do get the offset.
- **Findings:** the sky reads the view heading from a per-player table at `0x8c256570`
  (`0x8c256428 + 0x148`, stride 0x330; heading/pitch/roll s32). Readers: `0x8c04b82e` (passes it
  to its own camera build at `0x8c0539a0`), `0x8c04bf76`, `0x8c04c36e` (sin table at
  `0x8c200da4`). Writing the hooked angles there from the BuildCam stub had no visible effect,
  so the sky probably reads it before the gamecam setup runs. Writer not found yet; recovered
  `sky.h` declares `sky_Work(int nYaw, int nPitch, int nRoll)` - find its call site and hook
  its arguments.

### Sun visible inside tunnels (logged 2026-10-21, milestone 3)
- Not yet checked whether the unmodified game does the same (compare on desktop without hook).

### Sun appears at the wrong depth in stereo (logged 2026-10-21, Quest milestone 2)
- **Seen:** in the headset (3D virtual screen), the sun doesn't sit at the far distance.
- **Likely cause (unverified):** the sun / lens-flare sprites are drawn with a large 1/w, so the
  stereo renderer treats them as HUD (1/w >= `HT4Q_STEREO_HUDZ`, 0.75) and pins them to the HUD
  depth instead of infinity. Check with a frame dump (`drive.py dump:NAME`) near the sun.
- **Fix idea:** identify the sun/flare polygons (texture or list) and force them to 1/w = 0.
- **Update (milestone 3):** looks right in the immersive VR view; still to check on the 3D screen.
