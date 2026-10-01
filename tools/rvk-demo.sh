#!/bin/sh
# Run the rvk test scenes headless under Wine with the Khronos validation layer (host side, through
# winevulkan) and convert the screenshot to PNG.
# Usage: tools/rvk-demo.sh [out-dir] [demo args...]    (default out-dir: build/rvk-demo)
cd "$(dirname "$0")/.." || exit 1
out=${1:-build/rvk-demo}
[ $# -gt 0 ] && shift
mkdir -p "$out"
sdk=${VULKAN_SDK_DIR:-$HOME/repos/vulkan-sdk/1.4.321.1/x86_64}
exe=$PWD/build/linux-release/rvk_demo.exe
cd "$out" || exit 1
WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} WINEDEBUG=-all \
VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d" LD_LIBRARY_PATH="$sdk/lib" \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    timeout 120 wine "$exe" --frames 3 --shot rvk_demo.bmp "$@" 2>&1 |
    grep -v -iE 'pci id|EGL' |
    # winevulkan itself enables VK_EXT_external_memory_dma_buf without its dependency; not ours.
    awk 'BEGIN {RS = ""; ORS = "\n\n"} !/external_memory_dma_buf/' | tee validation.log
python3 "$OLDPWD/tools/bmp2png.py" rvk_demo.bmp rvk_demo.png && echo "wrote rvk_demo.png"
