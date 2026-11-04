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

## Driver setup with CrossOver

Do not modify the signed `CrossOver Preview.app`, because macOS then reports it as damaged. Instead, make an APFS clone
of its `Contents/SharedSupport/CrossOver` folder at a short path. Then set up the clone as follows:

1. Replace `lib/aarch64/libMoltenVK.dylib` with the Vulkan SDK's loader, an arm64 slice of `lib/libvulkan.1.x.dylib`.
2. Run the clone's `bin/wine` with
   `VK_DRIVER_FILES=<SDK>/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json`.

## Build on macOS

Install the tools:

- Homebrew `llvm@22` and `lld@22`
- `xwin`
- the Vulkan SDK for macOS, which provides headers, `glslc` and `spirv-cross`
- `vk_mem_alloc.h` from VMA 3.3.0, placed as `<dir>/vma/vk_mem_alloc.h`

Then build:

```bash
xwin --arch x86 --cache-dir ~/.xwin-cache splat --output ~/.xwin   # you accept Microsoft's license yourself
cmake -S . -B build/mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/xwin-mac.cmake \
      -DVULKAN_SDK_DIR=<SDK>/macOS -DVMA_INCLUDE_DIR=<dir>
cmake --build build/mac-release --target randy31
```
