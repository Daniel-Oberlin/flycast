#!/bin/sh
# Quest helper (needs the headset in developer mode, USB debugging allowed).
#   quest.sh build     build the quest debug APK
#   quest.sh install   install it
#   quest.sh push      copy the Rev 1 bin/cue to the app's own storage on the headset
#   quest.sh run       launch straight into Hydro Thunder; forwards the memserver to :3264
#   quest.sh log       follow Flycast's log
cd "$(dirname "$0")/../.." || exit 1
PKG=com.flycast.emulator.ht4q
APK=shell/android-studio/flycast/build/outputs/apk/quest/debug/flycast-quest-debug.apk
GAMEDIR="../HydroThunder4Quest-gamefiles/Hydro Thunder (USA) (Rev 1)"
DEVDIR=/sdcard/Android/data/$PKG/files/game
case "$1" in
build)   (cd shell/android-studio && ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk} ./gradlew assembleQuestDebug) ;;
install) adb install -r "$APK" ;;
push)    adb shell mkdir -p "$DEVDIR" && adb push "$GAMEDIR"/. "$DEVDIR/" ;;
run)     adb forward tcp:3264 tcp:3264
         adb shell am start -n $PKG/com.flycast.emulator.NativeGLActivity \
             -d "file://$DEVDIR/Hydro%20Thunder%20(USA)%20(Rev%201).cue" ;;
log)     adb logcat -s flycast:V ;;
*)       sed -n 2,8p "$0"; exit 1 ;;
esac
