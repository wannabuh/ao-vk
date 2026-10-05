#!/bin/sh
# Run tests/randy_harness.cpp against the installed client: the exe is copied into the client folder (so
# it loads the same randy31.dll proxy, randy31_orig.dll and ddraw.dll as the game), run there, removed.
# Usage: tools/randy-harness.sh [out-dir] [harness args...]     (default out-dir: build/randy-harness)
cd "$(dirname "$0")/.." || exit 1
repo=$PWD
out=${1:-build/randy-harness}
[ $# -gt 0 ] && shift
mkdir -p "$out"
out=$(cd "$out" && pwd)
client=${AO_CLIENT:-$HOME/.wine-prk/drive_c/linux/client}
# The Wine game process shows up as CrBrowserMain (CEF renames its main thread) or with a Windows path argv0;
# don't match ao-wine.sh, whose command line also contains AnarchyOnline.exe.
# A separate test client folder (AO_CLIENT, e.g. symlinks to the real one) can be used while the game runs.
if [ -z "$AO_CLIENT" ] && { pgrep -x CrBrowserMain >/dev/null || ps -eo args | grep -qE '^([A-Za-z]:\\[^ ]*\\)?AnarchyOnline\.exe'; }; then
    echo "game is running; not touching the client folder" >&2
    exit 1
fi
cp build/linux-release/randy_harness.exe "$client/randy_harness.exe" || exit 1
# Test the freshly built proxy; put back whatever was installed afterwards.
cp "$client/randy31.dll" "$client/randy31.dll.harness-bak" || exit 1
cp "${RANDY_DLL:-build/linux-release/randy31.dll}" "$client/randy31.dll"
cd "$client" || exit 1
# RANDYVK_DDRAW (e.g. trace) is passed through from the caller's environment.
rm -f ../logs/randy-vk-harness.log
WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} WINEDEBUG=-all WINEDLLOVERRIDES="msvcr100,msvcp100=n,b" \
DXVK_CONFIG_FILE="$client/../dxvk.conf" DXVK_LOG_LEVEL=warn RANDYVK_CAPTURE_FRAME=${RANDYVK_CAPTURE_FRAME:-3} \
RANDYVK_LOG="C:\\linux\\logs\\randy-vk-harness.log" \
    timeout 120 wine randy_harness.exe --shot randy_harness.bmp "$@" > "$out/wine-output.txt" 2>&1
# (to a file, not a pipe: Wine's services, still running for the next launch, keep their copy of it open)
grep -v -iE 'pci id|EGL' "$out/wine-output.txt"
rm -f "$client/randy_harness.exe"
mv "$client/randy31.dll.harness-bak" "$client/randy31.dll"
[ -f ../logs/randy-vk-harness.log ] && cp ../logs/randy-vk-harness.log "$out/randy-vk.log"
[ -f randy_harness.bmp ] && mv randy_harness.bmp "$out/" && python3 "$repo/tools/bmp2png.py" "$out/randy_harness.bmp" "$out/randy_harness.png" && echo "wrote $out/randy_harness.png"
