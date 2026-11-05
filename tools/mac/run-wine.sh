#!/bin/sh
# Run a Windows program in a CrossOver bottle, with the Wine copy made by tools/mac/setup-wine.sh and the KosmicKrisp
# Vulkan driver (docs/mac.md).
# Usage: tools/mac/run-wine.sh [--validation] <bottle> <program> [args...]
#   --validation  adds the Khronos validation layer (slow; for debugging). With VK_KHRONOS_VALIDATION_LOG_FILENAME set,
#                 its messages go to that file.
#   <program>     a Windows path, such as 'C:\PRKLauncher\PRK.Launcher.exe' (it then runs in its own folder), or a
#                 macOS path (it runs in the current folder).
# RVK_MAC_WINE names the Wine copy (default ~/ao-vk-wine), VULKAN_SDK the Vulkan SDK's macOS folder.
validation=
[ "$1" = --validation ] && { validation=1; shift; }
[ $# -ge 2 ] || { echo "usage: $0 [--validation] <bottle> <program> [args...]" >&2; exit 2; }
bottle=$1 prog=$2
shift 2
sdk=${VULKAN_SDK:?set VULKAN_SDK to the macOS folder of the Vulkan SDK (setup-env.sh there sets it)}
copy=$(cd "${RVK_MAC_WINE:-$HOME/ao-vk-wine}" 2>/dev/null && pwd -P) ||
    { echo "no Wine copy at ${RVK_MAC_WINE:-$HOME/ao-vk-wine}: run tools/mac/setup-wine.sh" >&2; exit 1; }

# A bottle that another Wine still runs (usually CrossOver itself) is not joined: the programs it started there use its
# MoltenVK, and a Wine of another version may not be able to talk to its wineserver. Wine keeps a bottle's server in
# /tmp/.wine-<uid>/server-<device>-<inode> of the bottle's folder. lsof can't find Unix sockets by path on macOS, so
# this looks for a wineserver working in that folder. A wineserver of the copy itself, left from an earlier run, is
# used again.
dir="$HOME/Library/Application Support/CrossOver/Bottles/$bottle"
[ -d "$dir" ] || { echo "no CrossOver bottle \"$bottle\" ($dir)" >&2; exit 1; }
server=/private/tmp/.wine-$(id -u)/$(printf 'server-%x-%x' $(stat -f '%d %i' "$dir"))
pid=$(lsof -a -c wineserver -d cwd -Fpn 2>/dev/null |
      awk -v want="n$server" '/^p/ { p = substr($0, 2) } $0 == want { print p; exit }')
if [ -n "$pid" ]; then
    exe=$(ps -o comm= -p "$pid")
    case $exe in
        "$copy"/*) ;;
        *) echo "the bottle \"$bottle\" is still running in another Wine ($exe): close its programs first" >&2
           exit 1 ;;
    esac
fi

export VK_DRIVER_FILES="$sdk/share/vulkan/icd.d/libkosmickrisp_icd.json"
if [ -n "$validation" ]; then
    export VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
case $prog in
    [A-Za-z]:\\*) set -- --workdir "${prog%\\*}" "$prog" "$@" ;;
    *) # (Wine runs a macOS path as Z:<path>, and Windows starts no program whose path is longer than 259 characters:
       # CrossOver's wrapper only says it "cannot execute" it.)
       full=$prog
       case $full in /*) ;; *) full=$PWD/$prog ;; esac
       if [ $((${#full} + 2)) -gt 259 ]; then
           echo "the program's path is too long for Windows ($((${#full} + 2)) characters as Z:..., at most 259):" \
                "copy it to a shorter folder" >&2
           exit 1
       fi
       set -- "$prog" "$@" ;;
esac
exec "$copy/bin/wine" --bottle "$bottle" "$@"
