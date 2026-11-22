#!/bin/bash
# [android] macOS (and Linux): your own disc -> the game library for your phone, in one command (or a double-click in
# Finder: a .command file opens in Terminal).
#
#     tools/build-android-pack.command [vcs|lcs] ["/path/to/your game.iso"]
#
# The twin of tools/build-android-pack.bat (the same steps, prompts and messages; docs/ANDROID.md "PC builder: Windows
# vs macOS", checked by tools/preflight_check.py). Without a disc it asks for one (drag the .iso into the window). Runs
# tools/android_pack.py, the same script as the launcher's "Prepare for Android" button: it checks the tools, offers to
# download the missing ones (it lists them with their sizes and asks y/N; tools/android.py setup), recompiles the game
# from your disc, builds the library with the Android NDK and copies it to android-export/<game>/. The game is read
# from the disc when not given.
# Python: the release's python/ folder if there is one, else python3 on PATH (Apple's Command Line Tools or Homebrew).
GAMEARG=()
ISO=""
ASKED=""
case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
    vcs|lcs) GAMEARG=(--game "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"); shift ;;
esac
ISO="$1"

pause_if_asked() {
    if [ -n "$ASKED" ]; then
        read -r -p "[pack] Press Enter to close this window. " _
    fi
}

usage() {
    echo "Usage: tools/build-android-pack.command [vcs|lcs] \"/path/to/your game.iso\""
    echo "  vcs = Vice City Stories (USA, SLUS_215.90)    lcs = Liberty City Stories (USA, SLUS_214.23); default: read from the disc"
    pause_if_asked
    exit 1
}

failed() {
    echo "[pack] Stopped: see the message above, then run this again."
    pause_if_asked
    exit 1
}

echo "[pack] Prepare for Android: your disc -> the game library for your phone (tools/android_pack.py)."
if [ -z "$ISO" ]; then
    ASKED=1
    echo "[pack] Drag your disc image (.iso) into this window, or type its path, then press Enter:"
    IFS= read -r ISO
    # a dragged path comes with backslash escapes (My\ Game.iso) and a trailing space, or in quotes
    ISO="$(printf '%s' "$ISO" | sed -e 's/[[:space:]]*$//' -e 's/^[[:space:]]*//' -e "s/^'\(.*\)'$/\1/" \
                                     -e 's/^"\(.*\)"$/\1/' -e 's/\\\(.\)/\1/g')"
    [ -z "$ISO" ] && usage
fi
if [ ! -f "$ISO" ]; then
    echo "[pack] The disc image was not found: $ISO"
    usage
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY=""
if [ -x "$ROOT/python/bin/python3" ]; then
    PY="$ROOT/python/bin/python3"
elif command -v python3 >/dev/null 2>&1 && python3 -c "import sys" >/dev/null 2>&1; then
    PY="python3"
else
    echo "[pack] No Python found: there is no python folder here and no Python 3 on this computer."
    echo "[pack] Install Python 3 from https://www.python.org/downloads/ (on Windows tick 'Add python.exe to PATH'; on a Mac also: xcode-select --install), then open a NEW window and run this again."
    failed
fi
echo "[pack] Python: $PY"
cd "$ROOT" || failed

"$PY" tools/android_pack.py "${GAMEARG[@]}" "$ISO" || failed

echo "[pack] Done: copy the file named above (libvcs_game.so.gz) to your phone and import it in the app: Disc > Import game library."
echo "[pack] It holds game code from your disc: for your own phone only, never share it."
pause_if_asked
exit 0
