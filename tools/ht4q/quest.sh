#!/bin/sh
# Quest helper (needs the headset in developer mode, USB debugging allowed).
#   quest.sh build     build the quest debug APK
#   quest.sh install   install it
#   quest.sh push [ct] copy the Rev 1 bin/cue (ct: the Crazy Taxi image) into the app's private
#                      storage, game/<game ID>/ (debug build; install.sh does both games)
#   quest.sh run [ct]  launch straight into Hydro Thunder (ct: Crazy Taxi); forwards the memserver to :3264
#   quest.sh log       follow Flycast's log
#   quest.sh shot F    save the screen being presented to the headset to F.png
#   quest.sh save F    savestate (slot 9) of the running game to F, to load on the desktop
#   quest.sh depth on|off   give the compositor per-pixel depth for reprojection (default on)
cd "$(dirname "$0")/../.." || exit 1
PKG=com.flycast.emulator.ht4q
APK=shell/android-studio/flycast/build/outputs/apk/quest/debug/flycast-quest-debug.apk
GAMES=/data/user/0/$PKG/files/game    # app-private; adb can't write here directly
if [ "$2" = ct ]; then
	SRC="../gamefiles/Crazy Taxi (USA).cdi" DEVDIR=$GAMES/RDC-0102 IMAGE="Crazy Taxi (USA).cdi"
else
	SRC="../gamefiles/Hydro Thunder (USA) (Rev 1)/." DEVDIR=$GAMES/T-9702N IMAGE="Hydro Thunder (USA) (Rev 1).cue"
fi
case "$1" in
build)   (cd shell/android-studio && ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk} ./gradlew assembleQuestDebug) ;;
install) adb install -r "$APK" ;;
push)    adb shell mkdir -p /data/local/tmp/ht4q && adb push "$SRC" /data/local/tmp/ht4q/ &&
         adb shell "run-as $PKG mkdir -p $DEVDIR && run-as $PKG cp -r /data/local/tmp/ht4q/. $DEVDIR/" &&
         adb shell rm -rf /data/local/tmp/ht4q ;;
run)     adb forward tcp:3264 tcp:3264
         adb shell "am start -a android.intent.action.VIEW -n $PKG/com.flycast.emulator.NativeGLActivity \
             -d '$DEVDIR/$IMAGE'" ;;
log)     adb logcat -s flycast:V ;;
shot)    adb shell touch "/sdcard/Android/data/$PKG/files/ht4q_shot.req"; sleep 1
         adb pull "/sdcard/Android/data/$PKG/files/ht4q_shot.png" "${2:-quest_shot.png}" ;;
save)    adb shell rm -f "/sdcard/Android/data/$PKG/files/ht4q_save.state"
         adb shell touch "/sdcard/Android/data/$PKG/files/ht4q_save.req"; sleep 4
         adb pull "/sdcard/Android/data/$PKG/files/ht4q_save.state" "${2:-quest.state}" ;;
depth)   case "$2" in
         on)  adb shell rm -f "/sdcard/Android/data/$PKG/files/ht4q_nodepth" ;;
         off) adb shell touch "/sdcard/Android/data/$PKG/files/ht4q_nodepth" ;;
         *)   echo "depth on|off"; exit 1 ;;
         esac ;;
*)       sed -n 2,10p "$0"; exit 1 ;;
esac
