#!/usr/bin/env bash
# Install HydroThunder4Quest on a Meta Quest, with your own disc images (Hydro Thunder, Crazy Taxi, 18 Wheeler).
#
#   tools/ht4q/install.sh [--roms DIR]... [--game FILE]... [--textures DIR [--textures-game ID]]
#                         [--apk FILE] [--no-launch]
#
#   --roms DIR      where to look for the disc images (repeatable; also $HT4Q_ROMS, colon-separated).
#                   Default places: ../gamefiles, ~/ROMs, ~/roms, ~/Games, ~/Downloads, ~/Desktop, ~/Documents
#   --game FILE     install this .cue / .gdi / .chd / .cdi instead of searching (repeatable)
#   --textures DIR  also install HQ textures: a folder of Flycast custom textures (make your own with
#                   tools/ht4q/hqtex.py) for the game --textures-game names (its game ID, e.g.
#                   T-9702N); without it, for the one game being installed
#   --apk FILE      install this APK instead of building one (needs the Android SDK)
#   --no-launch     don't start the app afterwards
#
# Each game goes in its own folder in the app (game/<game ID>/), replacing only that game's files:
# installing one game keeps the others. Pick the game in the app's launcher.
# Needs: adb (Android platform-tools), python3, the headset in developer mode, connected over USB with
# USB debugging allowed. Game files are never included: use dumps of discs you own. Head tracking
# needs the images the hooks were made for: "Hydro Thunder (USA) (Rev 1)" (T-9702N); others boot
# without it.
set -e
cd "$(dirname "$0")/../.."
PKG=com.flycast.emulator.ht4q
APK=shell/android-studio/flycast/build/outputs/apk/quest/debug/flycast-quest-debug.apk
PRIV=/data/user/0/$PKG/files
TMP=/data/local/tmp/ht4q_install

ROMDIRS=()
GAMES=() TEXTURES= TEXGAME= LAUNCH=1 BUILD=1
while [ $# -gt 0 ]; do
	case "$1" in
	--roms) ROMDIRS+=("$2"); shift 2 ;;
	--game) GAMES+=("$2"); shift 2 ;;
	--textures) TEXTURES=$2; shift 2 ;;
	--textures-game) TEXGAME=$2; shift 2 ;;
	--apk) APK=$2; BUILD=0; shift 2 ;;
	--no-launch) LAUNCH=0; shift ;;
	-h|--help) sed -n 2,23p "$0"; exit 0 ;;
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

# --- game images: the best image of each game we know, or the --game files
best_image() {	# $1: find -iname pattern
	local best= bestscore=-1 dir f lower score
	for dir in "${ROMDIRS[@]}"; do
		[ -d "$dir" ] || continue
		while IFS= read -r f; do
			lower=$(printf '%s' "$f" | tr '[:upper:]' '[:lower:]')
			score=0
			case "$lower" in *"rev 1"*) score=$((score + 2)) ;; esac
			case "$lower" in *.cue) score=$((score + 1)) ;; esac
			if [ $score -gt $bestscore ]; then best=$f; bestscore=$score; fi
		done < <(find "$dir" -maxdepth 4 -type f -iname "$1" \
				\( -iname '*.cue' -o -iname '*.gdi' -o -iname '*.chd' -o -iname '*.cdi' \) 2> /dev/null)
	done
	printf '%s' "$best"
}
if [ ${#GAMES[@]} = 0 ]; then
	for pattern in '*hydro*thunder*' '*crazy*taxi*' '*18*wheeler*'; do
		g=$(best_image "$pattern")
		[ -n "$g" ] && GAMES+=("$g")
	done
fi
[ ${#GAMES[@]} -gt 0 ] || die "no disc image found (Hydro Thunder, Crazy Taxi, 18 Wheeler); point to them with --roms DIR or --game FILE"

# per game: its folder on the headset (the game ID) and its files (the image plus the track files it refers to)
FOLDERS=() ALLFILES=() OWNER=()
for game in "${GAMES[@]}"; do
	[ -f "$game" ] || die "no such file: $game"
	id=$(python3 -I tools/ht4q/discid.py "$game" 2> /dev/null || true)
	folder=${id:-$(basename "${game%.*}" | tr -c 'A-Za-z0-9._-' '_')}
	echo "game: $game (${id:-game ID not read})"
	case "$id" in
	T-9702N|RDC-0102|RDC-0094) ;;
	*) echo "  note: no head tracking for this image - it boots, but plays like a flat screen" ;;
	esac
	gamedir=$(dirname "$game")
	files=("$game")
	case "$(printf '%s' "$game" | tr '[:upper:]' '[:lower:]')" in
	*.cue)
		while IFS= read -r t; do files+=("$gamedir/$t"); done < <(sed -n 's/^[[:space:]]*FILE[[:space:]]*"\(.*\)".*/\1/p' "$game" | tr -d '\r') ;;
	*.gdi)
		while IFS= read -r t; do files+=("$gamedir/$t"); done < <(tail -n +2 "$game" | tr -d '\r' |
			sed -nE 's/^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+("[^"]*"|[^[:space:]]+).*/\1/p' | tr -d '"') ;;
	esac
	for f in "${files[@]}"; do
		[ -f "$f" ] || die "missing track file: $f"
		ALLFILES+=("$f"); OWNER+=("$folder")
	done
	FOLDERS+=("$folder")
done
if [ -n "$TEXTURES" ] && [ -z "$TEXGAME" ]; then
	[ ${#FOLDERS[@]} = 1 ] || die "--textures: say which game they are for with --textures-game ID"
	TEXGAME=${FOLDERS[0]}
fi

# --- app
if [ $BUILD = 1 ] && [ ! -f "$APK" ]; then
	echo "building the APK (needs the Android SDK and NDK)..."
	(cd shell/android-studio && ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk} ./gradlew -q assembleQuestDebug)
fi
[ -f "$APK" ] || die "no APK at $APK"
echo "installing $APK"
adb install -r "$APK" > /dev/null

# --- copy into the app's private storage (adb can only write there through run-as)
echo "copying the games (${#ALLFILES[@]} files)..."
adb shell "rm -rf $TMP && mkdir -p $TMP/game"
for folder in "${FOLDERS[@]}"; do adb shell mkdir -p "'$TMP/game/$folder'"; done
for i in "${!ALLFILES[@]}"; do adb push "${ALLFILES[$i]}" "$TMP/game/${OWNER[$i]}/" > /dev/null; done
if [ -n "$TEXTURES" ]; then
	[ -d "$TEXTURES" ] || die "no texture folder $TEXTURES"
	echo "copying HQ textures..."
	adb shell mkdir -p "'$TMP/textures/$TEXGAME'"
	adb push "$TEXTURES/." "$TMP/textures/$TEXGAME/" > /dev/null
fi
# replace only these games' folders (and drop images an older install put directly in game/)
clear="run-as $PKG mkdir -p $PRIV/game && run-as $PKG find $PRIV/game -maxdepth 1 -type f -delete"
for folder in "${FOLDERS[@]}"; do clear="$clear && run-as $PKG rm -rf '$PRIV/game/$folder'"; done
adb shell "$clear && run-as $PKG cp -r $TMP/. $PRIV/ && rm -rf $TMP" \
	|| die "copying into the app failed (run-as needs the debug APK)"

if [ $LAUNCH = 1 ]; then
	adb shell am start -n $PKG/com.flycast.emulator.NativeGLActivity > /dev/null
	echo "started - put on the headset"
else
	echo "done - start HydroThunder4Quest from the Quest's app library (Unknown sources) and pick the game"
fi
