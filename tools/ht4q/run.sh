#!/bin/sh
# Launch the self-built Flycast with Hydro Thunder (USA) (Rev 1) (HT4Q_GAME=ct: Crazy Taxi, 18w: 18 Wheeler) and the
# ht4q memserver on :3264. Muted by default (automated runs); HT4Q_VOLUME=100 for sound.
# Flycast ignores options after the game, so extra arguments go before it.
cd "$(dirname "$0")/../.." || exit 1
case "$HT4Q_GAME" in
ct) GAME="../gamefiles/Crazy Taxi (USA).cdi" ;;
18w) GAME="../gamefiles/18 Wheeler - American Pro Trucker (USA).cdi" ;;
*)  GAME="../gamefiles/Hydro Thunder (USA) (Rev 1)/Hydro Thunder (USA) (Rev 1).cue" ;;
esac
HT4Q_MEMSERVER=${HT4Q_MEMSERVER:-3264} exec build/RelWithDebInfo/Flycast.app/Contents/MacOS/Flycast \
  -config aica:Volume=${HT4Q_VOLUME:-0} "$@" "$GAME"
