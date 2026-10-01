#!/bin/sh
# Run the rvk test scenes headless under Wine with the Khronos validation layer (host side, through
# winevulkan) and convert the screenshot to PNG.
# Usage: tools/rvk-demo.sh [out-dir]    (default: build/rvk-demo)
cd "$(dirname "$0")/.." || exit 1
out=${1:-build/rvk-demo}
mkdir -p "$out"
sdk=${VULKAN_SDK_DIR:-$HOME/repos/vulkan-sdk/1.4.321.1/x86_64}
exe=$PWD/build/linux-release/rvk_demo.exe
cd "$out" || exit 1
WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} WINEDEBUG=-all \
VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d" LD_LIBRARY_PATH="$sdk/lib" \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    timeout 120 wine "$exe" --frames 3 --shot rvk_demo.bmp 2>&1 |
    grep -v -iE 'pci id|EGL' |
    # winevulkan itself enables VK_EXT_external_memory_dma_buf without its dependency; not ours.
    awk '/external_memory_dma_buf/ {skip = 1} /^Validation Error/ && !/01387/ {skip = 0} !skip' | tee validation.log
python3 - <<'PY'
import struct, zlib
d = open("rvk_demo.bmp", "rb").read()
w, h = struct.unpack_from("<ii", d, 18)
off = struct.unpack_from("<I", d, 10)[0]
row = (w * 3 + 3) & ~3
raw = bytearray()
for y in range(h):
    raw.append(0)
    line = d[off + (h - 1 - y) * row: off + (h - 1 - y) * row + w * 3]
    for x in range(w):
        raw += bytes((line[x * 3 + 2], line[x * 3 + 1], line[x * 3]))
chunk = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
open("rvk_demo.png", "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                                 + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
print("wrote rvk_demo.png")
PY
