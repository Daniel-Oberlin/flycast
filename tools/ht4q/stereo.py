"""Set stereo parameters live (desktop or Quest via `adb forward tcp:3264 tcp:3264`).

    python3 -I stereo.py SEP [CONV [HUDZ [HUD [UNITS_PER_METER [HUD_SHRINK_VR [SKY_CORRECT [VR_FOV]]]]]]]
    python3 -I stereo.py sky on|off|PX_PER_RADIAN
SEP  per-eye shift at infinity, NDC (Quest default 0.0267 = true infinity on the 2.4 m screen)
CONV convergence depth in game units (default 20: the chase-view boat sits on the screen plane)
HUDZ 1/w at or above which a vertex is HUD (0.75); HUD depth the HUD is pinned to (Quest 35)
UNITS_PER_METER game world scale for head tracking and eye separation in VR (3.28 = feet)
HUD_SHRINK_VR   in VR, scale the HUD toward the view center by 1 - this (0.35)
SKY_CORRECT     VR sky re-projection: the game's sky px per radian of turn (270), 0 = off
VR_FOV          horizontal field of view in the headset, degrees (116; applies at the next camera setup)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem

if len(sys.argv) < 2:
    sys.exit(__doc__)
defaults = [0.0267, 20.0, 0.75, 35.0, 3.28, 0.35, 270.0, 116.0]
if sys.argv[1] == 'sky':
    sys.argv[1:] = [str(x) for x in defaults[:6]] + [{'on': '270', 'off': '0'}.get(sys.argv[2], sys.argv[2])]
v = [float(x) for x in sys.argv[1:9]]
DcMem().stereo(*(v + defaults[len(v):]))
