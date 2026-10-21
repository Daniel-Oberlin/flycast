#!/bin/sh
# Launch the self-built Flycast with Hydro Thunder (USA) (Rev 1) and the ht4q memserver on :3264.
cd "$(dirname "$0")/../.." || exit 1
HT4Q_MEMSERVER=${HT4Q_MEMSERVER:-3264} exec build/RelWithDebInfo/Flycast.app/Contents/MacOS/Flycast \
  "../HydroThunder4Quest-gamefiles/Hydro Thunder (USA) (Rev 1)/Hydro Thunder (USA) (Rev 1).cue" "$@"
