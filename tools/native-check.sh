#!/bin/sh
# Run tests/native_check.cpp under Wine against the client's randy31_orig.dll (default: the test client folder,
# so it works while the game runs).
cd "$(dirname "$0")/.." || exit 1
client=${AO_CLIENT:-$HOME/.wine-prk/drive_c/linux/testclient}
WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} WINEDEBUG=-all \
    timeout 300 wine build/linux-release/native_check.exe "$(winepath -w "$client/randy31_orig.dll")" "$@" 2>&1 |
    grep -v -iE 'pci id|EGL'
