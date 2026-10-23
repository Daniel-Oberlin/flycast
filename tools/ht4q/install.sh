#!/usr/bin/env bash
# Install HydroThunder4Quest on a Meta Quest, with your own Hydro Thunder disc image.
#
#   tools/ht4q/install.sh [--roms DIR]... [--textures DIR] [--apk FILE] [--no-launch]
#
#   --roms DIR      where to look for the disc image (repeatable; also $HT4Q_ROMS, colon-separated).
#                   Default places: ../gamefiles, ~/ROMs, ~/roms, ~/Games, ~/Downloads, ~/Desktop, ~/Documents
#   --game FILE     use this .cue / .gdi / .chd / .cdi instead of searching
#   --textures DIR  also install HQ textures: a folder of Flycast custom textures for T-9702N
#                   (make your own with tools/ht4q/hqtex.py)
#   --apk FILE      install this APK instead of building one (needs the Android SDK)
#   --no-launch     don't start the game afterwards
#
# Needs: adb (Android platform-tools), the headset in developer mode, connected over USB with
# USB debugging allowed. Game files are never included: use a dump of a disc you own. Head tracking
# needs the "Hydro Thunder (USA) (Rev 1)" image; others boot but the camera hook stays off.
set -e
cd "$(dirname "$0")/../.."
PKG=com.flycast.emulator.ht4q
APK=shell/android-studio/flycast/build/outputs/apk/quest/debug/flycast-quest-debug.apk
PRIV=/data/user/0/$PKG/files
TMP=/data/local/tmp/ht4q_install

ROMDIRS=()
GAME= TEXTURES= LAUNCH=1 BUILD=1
while [ $# -gt 0 ]; do
	case "$1" in
	--roms) ROMDIRS+=("$2"); shift 2 ;;
	--game) GAME=$2; shift 2 ;;
	--textures) TEXTURES=$2; shift 2 ;;
	--apk) APK=$2; BUILD=0; shift 2 ;;
	--no-launch) LAUNCH=0; shift ;;
	-h|--help) sed -n 2,18p "$0"; exit 0 ;;
	*) echo "unknown option $1 (see --help)"; exit 1 ;;
	esac
done
if [ -n "$HT4Q_ROMS" ]; then
	IFS=: read -r -a extra <<< "$HT4Q_ROMS"
	ROMDIRS+=("${extra[@]}")
fi
ROMDIRS+=(../gamefiles "$HOME/ROMs" "$HOME/roms" "$HOME/Games" "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents")

die() { echo "error: $*" >&2; exit 1; }

# --- headset
command -v adb > /dev/null || die "adb not found: install Android platform-tools"
state=$(adb get-state 2> /dev/null || true)
[ "$state" = device ] || die "no headset: connect it over USB, enable developer mode and allow USB debugging (adb devices shows: ${state:-nothing})"

# --- game image
if [ -z "$GAME" ]; then
	best= bestscore=-1
	for dir in "${ROMDIRS[@]}"; do
		[ -d "$dir" ] || continue
		while IFS= read -r f; do
			lower=$(printf '%s' "$f" | tr '[:upper:]' '[:lower:]')
			score=0
			case "$lower" in *"rev 1"*) score=$((score + 2)) ;; esac
			case "$lower" in *.cue) score=$((score + 1)) ;; esac
			if [ $score -gt $bestscore ]; then best=$f; bestscore=$score; fi
		done < <(find "$dir" -maxdepth 4 -type f -iname '*hydro*thunder*' \
				\( -iname '*.cue' -o -iname '*.gdi' -o -iname '*.chd' -o -iname '*.cdi' \) 2> /dev/null)
	done
	GAME=$best
fi
[ -n "$GAME" ] && [ -f "$GAME" ] || die "Hydro Thunder disc image not found; point to it with --roms DIR or --game FILE"
echo "game: $GAME"
case "$GAME" in *"(Rev 1)"*) ;; *) echo "note: not the Rev 1 image - it will boot, but without head tracking" ;; esac

# the image plus the track files it refers to
gamedir=$(dirname "$GAME")
files=("$GAME")
case "$(printf '%s' "$GAME" | tr '[:upper:]' '[:lower:]')" in
*.cue)
	while IFS= read -r t; do files+=("$gamedir/$t"); done < <(sed -n 's/^[[:space:]]*FILE[[:space:]]*"\(.*\)".*/\1/p' "$GAME" | tr -d '\r') ;;
*.gdi)
	while IFS= read -r t; do files+=("$gamedir/$t"); done < <(tail -n +2 "$GAME" | tr -d '\r' |
		sed -nE 's/^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+("[^"]*"|[^[:space:]]+).*/\1/p' | tr -d '"') ;;
esac
for f in "${files[@]}"; do [ -f "$f" ] || die "missing track file: $f"; done

# --- app
if [ $BUILD = 1 ] && [ ! -f "$APK" ]; then
	echo "building the APK (needs the Android SDK and NDK)..."
	(cd shell/android-studio && ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk} ./gradlew -q assembleQuestDebug)
fi
[ -f "$APK" ] || die "no APK at $APK"
echo "installing $APK"
adb install -r "$APK" > /dev/null

# --- copy into the app's private storage (adb can only write there through run-as)
echo "copying the game (${#files[@]} files)..."
adb shell "rm -rf $TMP && mkdir -p $TMP/game"
for f in "${files[@]}"; do adb push "$f" "$TMP/game/" > /dev/null; done
if [ -n "$TEXTURES" ]; then
	[ -d "$TEXTURES" ] || die "no texture folder $TEXTURES"
	echo "copying HQ textures..."
	adb shell mkdir -p "$TMP/textures/T-9702N"
	adb push "$TEXTURES/." "$TMP/textures/T-9702N/" > /dev/null
fi
adb shell "run-as $PKG rm -rf $PRIV/game && run-as $PKG mkdir -p $PRIV && run-as $PKG cp -r $TMP/. $PRIV/ && rm -rf $TMP" \
	|| die "copying into the app failed (run-as needs the debug APK)"

if [ $LAUNCH = 1 ]; then
	adb shell am start -n $PKG/com.flycast.emulator.NativeGLActivity > /dev/null
	echo "started - put on the headset"
else
	echo "done - start Hydro Thunder from the Quest's app library (Unknown sources)"
fi
