# ao-vk on macOS (CrossOver / Wine on Apple Silicon)

## What the renderer needs from the driver

Wine passes the game's 32-bit Vulkan calls to a Mac Vulkan driver. Two drivers are available:

| | MoltenVK 1.2 (CrossOver's) | MoltenVK 1.4 (Vulkan SDK) | KosmicKrisp (Vulkan SDK, Mesa) |
|---|---|---|---|
| Vulkan version | 1.2 | 1.3 | 1.4 |
| `VK_EXT_vertex_input_dynamic_state` | no | no | no |
| EDS3 colour blend enable/equation | no | no | no |
| robustness2 `nullDescriptor` (bindless) | no | no | yes |

The renderer requires Vulkan 1.3 and bindless textures. That rules out CrossOver's own MoltenVK and MoltenVK 1.4.
KosmicKrisp has everything except the dynamic vertex input and the dynamic blend states.

## Dynamic state emulation (`rvk/dynemu.cpp`)

The renderer sets the vertex input and the colour blend enable and equation per draw. When the driver lacks these,
`CreateLogicalDevice` turns on the emulation, and the startup log says so:
`dynamic vertex input: emulated; dynamic blend enable and equation: emulated`.

The emulation works as follows:

- A pipeline made with those states dynamic becomes a template. The emulation keeps a copy of its create info behind
  a handle of its own.
- `vkCmdSetVertexInputEXT`, `vkCmdSetColorBlendEnableEXT` and `vkCmdSetColorBlendEquationEXT` are recorded for each
  command buffer.
- Before each draw, the emulation binds the template's variant for the recorded state. The variant is compiled the
  first time that state occurs. Compilation goes through the renderer's pipeline cache, which is saved to disk, so
  later sessions compile less.
- The draw code does not change. The emulation replaces the function pointers in `rvk::vk` after `LoadDevice`.

The emulation has these costs and limits:

- The first time a new combination of vertex format and blend mode appears, the frame stutters briefly.
- The shader register report (`ShaderRegisterReport`) is off while the emulation runs.

## Tessellation off on KosmicKrisp

With KosmicKrisp, a tessellated character up close burst into screen-sized spikes. This was seen in the character
selection, where only the selected character is tessellated (`RVK_TessDist`). Turning `RVK_TessOn` off fixed it.

The cause is not confirmed. A likely one is that the driver's tessellation path reads the wrong vertices when a draw
starts at a base vertex (`vertexOffset`) in a shared vertex buffer, as the GPU-skinned characters do.

On this driver, ao-vk therefore leaves tessellation off for characters, the native ground and the water surface.
They are drawn as they would be on a GPU without tessellation. `RANDYVK_TESS=1` turns tessellation back on, for
testing a newer driver.

## Emulation switch

The environment variable `RANDYVK_DYNAMIC_EMULATION` controls the emulation:

- `0` never emulates. Device creation then fails without these states, as it did before the emulation existed.
- `1` always emulates. Use this to test the emulation on a GPU that has the states.

## Wine with KosmicKrisp

CrossOver's Wine uses its own MoltenVK. Do not modify the signed `CrossOver Preview.app`, because macOS then reports
it as damaged. `tools/mac/setup-wine.sh` makes a copy of the app's Wine instead. The copy is an APFS clone, which
uses no disk space until files in it change. In the copy, the Vulkan SDK's loader replaces
`lib/aarch64/libMoltenVK.dylib`, and CrossOver's MoltenVK stays next to it as `libMoltenVK.crossover.dylib`:

```bash
source <SDK>/setup-env.sh            # sets VULKAN_SDK to <SDK>/macOS
tools/mac/setup-wine.sh              # copy in ~/ao-vk-wine, from "/Applications/CrossOver Preview.app"
```

The first argument names another folder for the copy, the second another CrossOver app. Run the script again after
a Vulkan SDK update. After a CrossOver update, delete the copy and run it again. The copy only serves ARM64 bottles,
since bottles that run x86_64 Wine load `lib/x86_64`.

`tools/mac/run-wine.sh` runs a program in a bottle with the copy and the KosmicKrisp driver (`VK_DRIVER_FILES`). For
the game, start the launcher:

```bash
tools/mac/run-wine.sh "PRK Preview ARM64" 'C:\PRKLauncher\PRK.Launcher.exe'
```

A Windows path runs the program in its own folder. A macOS path runs it in the current folder. Wine sees it as
`Z:<path>`, and Windows starts no program whose path is longer than 259 characters, so the script stops when it is.
`--validation` adds the Khronos validation layer. `RVK_MAC_WINE` names another copy than `~/ao-vk-wine`.

The script does not join a bottle that another Wine still runs, usually CrossOver itself. The programs that Wine
started there use its MoltenVK, and a Wine of another version may not be able to talk to its wineserver. The script
then stops with a message. Close the bottle's programs in CrossOver first.

Install ao-vk as on Windows: the game's `randy31.dll` renamed to `randy31_orig.dll`, ao-vk's `randy31.dll` in its
place, both in the client folder.

## Build on macOS

Install the tools:

- Homebrew `llvm@22` and `lld@22` (or the unversioned `llvm` and `lld`)
- `xwin`
- the Vulkan SDK for macOS, which provides headers, `glslc` and `spirv-cross`
- `vk_mem_alloc.h` from VMA 3.3.0, placed as `<dir>/vma/vk_mem_alloc.h`

Then build:

```bash
xwin --arch x86 --cache-dir ~/.xwin-cache splat --output ~/.xwin   # you accept Microsoft's license yourself
cmake --preset mac-release -DVMA_INCLUDE_DIR=<dir>                 # VULKAN_SDK set, as above
cmake --build --preset mac-release
```

The output is in `build/mac-release/`, as on Linux. `cmake/xwin-mac.cmake` finds Homebrew's LLVM and lld under
`/opt/homebrew` and `/usr/local`. `-DLLVM_BIN=` and `-DLLD_BIN=` name other folders.

## Testing on macOS

`tools/mac/rvk-demo.sh` is the macOS form of `tools/rvk-demo.sh`. It runs `rvk_demo` with KosmicKrisp and the
validation layer, in a bottle you name:

```bash
RVK_DEMO_BOTTLE="PRK Preview ARM64" tools/mac/rvk-demo.sh build/rvk-demo --native-ground-test --native-on
```

It writes `rvk_demo.png`, the program's output to `demo.log` and the layer's messages to `validation.log`, and counts
the messages by VUID. KosmicKrisp reports `VUID-vkDestroyDevice-device-05137` at every shutdown.

On the Mac, a GPU hang in one process makes macOS abort the GPU work of other processes too. A test that hangs the GPU
can make a running game lose its device, and ao-vk then stops rendering. Close the game before running GPU tests.

`tools/look-check.py`, `tools/native-check.sh` and `tools/calllog-ab.sh` are not ported to macOS yet.
