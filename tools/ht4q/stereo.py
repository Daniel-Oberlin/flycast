"""Set stereo parameters live (desktop or Quest via `adb forward tcp:3264 tcp:3264`).

    python3 -I stereo.py SEP [CONV [HUDZ [HUD [UNITS_PER_METER]]]]
SEP  per-eye shift at infinity, NDC (Quest default 0.0267 = true infinity on the 2.4 m screen)
CONV convergence depth in game units (default 20: the chase-view boat sits on the screen plane)
HUDZ 1/w at or above which a vertex is HUD (0.75); HUD depth the HUD is pinned to (15)
UNITS_PER_METER game world scale for head tracking and eye separation in VR (3.28 = feet)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dcmem import DcMem

if len(sys.argv) < 2:
    sys.exit(__doc__)
v = [float(x) for x in sys.argv[1:6]]
defaults = [0.0267, 20.0, 0.75, 15.0, 3.28]
DcMem().stereo(*(v + defaults[len(v):]))
