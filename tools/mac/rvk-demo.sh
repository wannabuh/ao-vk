#!/bin/sh
# tools/rvk-demo.sh on macOS: the rvk test scenes in a CrossOver bottle with KosmicKrisp and the Khronos validation
# layer (tools/mac/run-wine.sh). Writes rvk_demo.png, the program's output to demo.log and the layer's messages to
# validation.log, and counts the messages by VUID. KosmicKrisp reports VUID-vkDestroyDevice-device-05137 at every
# shutdown.
# Usage: RVK_DEMO_BOTTLE=<bottle> tools/mac/rvk-demo.sh [out-dir] [demo args...]    (default out-dir: build/rvk-demo)
# A GPU hang in a test can make other programs lose their GPU work too. When the game runs on the same Mac, it may lose
# its device.
cd "$(dirname "$0")/../.." || exit 1
root=$PWD
out=${1:-build/rvk-demo}
[ $# -gt 0 ] && shift
bottle=${RVK_DEMO_BOTTLE:?set RVK_DEMO_BOTTLE to the CrossOver bottle (an ARM64 one) to run the demo in}
exe=${RVK_DEMO_EXE:-$root/build/mac-release/rvk_demo.exe}
mkdir -p "$out" && cd "$out" || exit 1
rm -f rvk_demo.bmp rvk_demo.png validation.log
limit=
command -v timeout >/dev/null && limit="timeout 120"
VK_KHRONOS_VALIDATION_LOG_FILENAME=$PWD/validation.log VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG \
    $limit "$root/tools/mac/run-wine.sh" --validation "$bottle" "$exe" --frames 3 --shot rvk_demo.bmp "$@" 2>&1 |
    grep -v -E '^\*\*\*\*\*|^(Starting|CXConfig|Found no)' | tee demo.log
[ -f validation.log ] && grep -o 'VUID-[A-Za-z0-9_-]*' validation.log | sort | uniq -c
[ -f rvk_demo.bmp ] && python3 "$root/tools/bmp2png.py" rvk_demo.bmp rvk_demo.png && echo "wrote rvk_demo.png"
