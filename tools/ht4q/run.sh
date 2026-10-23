#!/bin/sh
# Launch the self-built Flycast with Hydro Thunder (USA) (Rev 1) and the ht4q memserver on :3264.
# Muted by default (automated runs); HT4Q_VOLUME=100 for sound.
cd "$(dirname "$0")/../.." || exit 1
HT4Q_MEMSERVER=${HT4Q_MEMSERVER:-3264} exec build/RelWithDebInfo/Flycast.app/Contents/MacOS/Flycast \
  -config aica:Volume=${HT4Q_VOLUME:-0} \
  "../gamefiles/Hydro Thunder (USA) (Rev 1)/Hydro Thunder (USA) (Rev 1).cue" "$@"
