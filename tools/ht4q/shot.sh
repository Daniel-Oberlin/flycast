#!/bin/sh
# Screenshot of the whole screen, downscaled to 1200px wide. Usage: shot.sh out.png
screencapture -x "$1.full.png" && sips -Z 1200 "$1.full.png" --out "$1" >/dev/null && rm "$1.full.png"
