# Known issues

## Open

### Sun appears at the wrong depth in stereo (logged 2026-10-21, Quest milestone 2)
- **Seen:** in the headset (3D virtual screen), the sun doesn't sit at the far distance.
- **Likely cause (unverified):** the sun / lens-flare sprites are drawn with a large 1/w, so the
  stereo renderer treats them as HUD (1/w >= `HT4Q_STEREO_HUDZ`, 0.75) and pins them to the HUD
  depth instead of infinity. Check with a frame dump (`drive.py dump:NAME`) near the sun.
- **Fix idea:** identify the sun/flare polygons (texture or list) and force them to 1/w = 0.
