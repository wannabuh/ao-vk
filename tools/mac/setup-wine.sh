#!/bin/sh
# Make a copy of CrossOver's Wine whose Vulkan calls go to the Vulkan SDK's loader, and from there to KosmicKrisp, for
# ao-vk on macOS (docs/mac.md). CrossOver itself stays as it is: the app is signed, and macOS reports a changed copy
# of it as damaged.
# Usage: VULKAN_SDK=<SDK>/macOS tools/mac/setup-wine.sh [copy-dir] [CrossOver app]
#   copy-dir       default $RVK_MAC_WINE, else ~/ao-vk-wine
#   CrossOver app  default "/Applications/CrossOver Preview.app"
# Run it again after a Vulkan SDK update to put the new loader in. After a CrossOver update, delete the copy and run
# it again.
set -e
copy=${1:-${RVK_MAC_WINE:-$HOME/ao-vk-wine}}
app=${2:-/Applications/CrossOver Preview.app}
sdk=${VULKAN_SDK:?set VULKAN_SDK to the macOS folder of the Vulkan SDK (setup-env.sh there sets it)}
loader=$sdk/lib/libvulkan.1.dylib
[ -f "$loader" ] || { echo "no Vulkan loader at $loader" >&2; exit 1; }
[ -f "$sdk/share/vulkan/icd.d/libkosmickrisp_icd.json" ] ||
    { echo "this Vulkan SDK has no KosmicKrisp (share/vulkan/icd.d/libkosmickrisp_icd.json)" >&2; exit 1; }

if [ ! -e "$copy" ]; then
    src=$app/Contents/SharedSupport/CrossOver
    [ -x "$src/bin/wine" ] || { echo "no CrossOver Wine in $src" >&2; exit 1; }
    # An APFS clone (cp -c): it takes no disk space until files in it change.
    cp -c -R "$src" "$copy"
    echo "copied $src to $copy"
fi
lib=$copy/lib/aarch64
[ -x "$copy/bin/wine" ] && [ -d "$lib" ] || { echo "$copy is not a copy of CrossOver's Wine" >&2; exit 1; }

# CrossOver's Wine on Apple Silicon loads lib/aarch64/libMoltenVK.dylib as its Vulkan library. The loader takes its
# place: its arm64 part, as the SDK's is universal. CrossOver's MoltenVK stays next to it, as
# libMoltenVK.crossover.dylib.
[ -e "$lib/libMoltenVK.crossover.dylib" ] || mv "$lib/libMoltenVK.dylib" "$lib/libMoltenVK.crossover.dylib"
rm -f "$lib/libMoltenVK.dylib"
lipo -thin arm64 "$loader" -output "$lib/libMoltenVK.dylib" 2>/dev/null || cp "$loader" "$lib/libMoltenVK.dylib"
echo "put the Vulkan loader of $sdk into $copy; run programs with tools/mac/run-wine.sh"
