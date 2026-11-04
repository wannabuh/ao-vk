# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ao-vk (files still named randy-vk: `randy-vk.ini`, `randy-vk.log`, `RANDYVK_*` env vars) is a drop-in replacement
for Anarchy Online's renderer DLL `randy31.dll`: a 32-bit Windows DLL, cross-compiled on Linux and run under Wine.
The game's original renderer is renamed `randy31_orig.dll` and still loaded; ao-vk sits under it (its Direct3D 7 /
DirectDraw calls go to a D3D7-shaped Vulkan renderer) and is replacing its functions one by one with native code.

## Build

Cross-compile with clang-cl + lld-link against the MSVC CRT / Windows SDK unpacked by `xwin splat` into `~/.xwin`
(`cmake/xwin-linux.cmake`), plus the Linux Vulkan SDK (glslc, headers, VMA):

```sh
cmake --preset linux-release -DVULKAN_SDK_DIR=/path/to/vulkan-sdk/x86_64   # default ~/repos/vulkan-sdk/1.4.321.1/x86_64
cmake --build --preset linux-release
```

Output in `build/linux-release/`: `randy31.dll` (+ `randy31.map`), and the test exes `rvk_demo`, `native_check`,
`crash_check`, `randy_harness`, `forward_check`. `compile_commands.json` is symlinked from that build dir.

On macOS (CrossOver's Wine with the KosmicKrisp driver), the preset is `mac-release` (`cmake/xwin-mac.cmake`, with
Homebrew's LLVM and lld). Set `VULKAN_SDK` to the macOS Vulkan SDK and pass `-DVMA_INCLUDE_DIR=<dir>`.
`docs/mac.md` covers the Wine setup and `tools/mac/` (`setup-wine.sh`, `run-wine.sh`, `rvk-demo.sh`).

Source files and shaders are listed explicitly in `CMakeLists.txt`: a new `.cpp` must be added to its target
(`randy_native`, `randy31` or `rvk`), a new shader to the glslc `foreach` list (or the define-variant list), and a new
shared `.glsl` include to the `DEPENDS` lists. Shaders compile to `build/.../rvk-shaders/*.inc` and are embedded.

## Testing

Everything runs under Wine (`WINEPREFIX` defaults to `~/.wine-prk`). There is no unit-test framework; the checks are:

- `tools/rvk-demo.sh [out-dir] [args]`: rvk alone, headless, with the Khronos validation layer; writes a PNG and
  `validation.log`. It runs in a Wine prefix of its own (`RVK_DEMO_PREFIX`, default `~/.wine`): the game's keeps a
  wineserver on the desktop's display while the launcher is open, and hidden demos joining it hang. Scene flags are in `tests/rvk_demo.cpp` (`--shadow-test`, `--grass-walk X`, `--tess 0.75`, ...).
  On macOS: `RVK_DEMO_BOTTLE=<bottle> tools/mac/rvk-demo.sh [out-dir] [args]`. Don't run it while the game runs on the
  same Mac, because a GPU hang there makes the game lose its device too.
- `tools/look-check.py [scene...]`: a few fixed rvk_demo scenes (dusk sweep, nights at three darkness levels, storm,
  the native ground) reduced to region averages and compared with `tests/look-baseline.tsv`; flags any region that
  moved and any validation message besides the known shutdown leaks. Run it after any change that shouldn't alter the
  look; after one that should, `--update` and commit the new baseline with it.
- `tools/native-check.sh`: each native replacement vs. the original function on the same synthetic input
  (`tests/native_check.cpp`), against the test client's `randy31_orig.dll`.
- `tools/calllog-ab.sh <Mode> [scene...]`: runs `randy_harness` scenes with `[Native] <Mode>=off` and `=on` and
  requires identical Direct3D call logs of one frame. Pass scene names (e.g. `blend shadow statics`) to run only
  those; the full list is in the script.
- `tools/randy-harness.sh [out-dir] [args]`: boots the real renderer through the proxy outside the game
  (`--character`, `--crowd N`, `--static`, `--pick`, ... see `tests/randy_harness.cpp` / `docs/native.md`). Character
  and static inputs come from `tools/extract-character.py` / `extract-static.py` into `build/characters`,
  `build/statics`. It copies exes into the client folder and refuses to run while the game runs unless `AO_CLIENT`
  points at a separate test client (`~/.wine-prk/drive_c/linux/testclient`).
- `tools/crash-check.sh`: crash / hang reporting.
- `tools/port-status.py`: porting progress against `docs/port-ledger.tsv`.

## Staging a build for the game

**Every change that touches the DLL gets staged for the game right away, without being asked**, so the user's next
launch runs it:

1. Build (`cmake --build --preset linux-release`).
2. Copy `build/linux-release/randy31.dll` and `randy31.map` to `~/.wine-prk/drive_c/linux/randy-vk/` as
   `randy31-<name>.dll` / `.map`, with a short new name for what changed (`randy31-albedo.dll`, `randy31-neon16.dll`;
   a later round of the same work gets a number). Never overwrite a staged DLL the game may be running.
3. In `~/.wine-prk/drive_c/linux/ao-wine.sh` (the launcher; not `~/Games/anarchy-online/ao-wine.sh`), point
   `AO_RANDYVK_DLL=` at it, its comment saying what the build adds and which DLL it replaced
   (`# randy-vk main + albedo maps keep colour under alpha 0 (was randy31-neon16.dll)`). Change only that line.
4. Tell the user the staged name; it takes effect at the next launch (ao-wine.sh copies it over `client/randy31.dll`).

## Architecture

Three layers, see `docs/pipeline.md` (diagrams: the layers, a frame on the GPU, the per-frame world state),
`docs/architecture.md`, `docs/frame.md`, `docs/rvk.md`, `docs/native.md`:

- **`proxy/`** — the `randy31.dll` proxy. It exports the same 771 functions as the original (`proxy/randy31.def`,
  `exports.gen.cpp` thunks); unported exports forward to `randy31_orig.dll`. `trace.cpp` loads the original and runs
  the native install. `RANDYVK_DDRAW=rvk|off|trace` picks the backend.
- **`proxy/ddraw/`** — DirectDraw / Direct3D 7 COM objects implemented on rvk (`rvk_ddraw.cpp`, `rvk_device.cpp`,
  `rvk_surface.cpp`), plus feature glue (weather, smoke, particles, statels, freecam, input, console). All user
  settings are one table in `rvk_settings.cpp` (persisted to `randy-vk.ini` `[Renderer]`, hot-reloaded, exported as a
  C interface `RvkSettings_*` that AOReloaded's options window uses; `kHidden` keeps a setting ini-only).
- **`rvk/`** — static lib: a Vulkan 1.3 renderer with an `IDirect3DDevice7`-shaped front end (same enums/layouts).
  One fixed-function uber-shader (`shaders/ffp.*`), no runtime pipeline compiles (everything dynamic state), and the
  enhancements (shadows, HDR/post, grass, water, clouds, smoke, ...) as passes around it. `threaded.cpp` runs the
  Vulkan work off the game thread.
- **`proxy/native/`** — static lib `randy_native`: native rewrites of the original's functions. `HookEntry` /
  `Replace` (`native.h`, `hook.cpp`) patch a jump over the original entry once the client build is recognised;
  `HookSlot` patches vtable slots. Each part has a `[Native]` mode in `randy-vk.ini` (`off` / `on` / often `verify`,
  which runs both and logs differences). Unported callees are reached through `orig_api.gen.h`; game heap /
  containers through `vc10.h` (VS2010 STL layouts).

Rules from the October 2026 audit (`docs/audit-2026-10.md`):

- **World state.** Features read the time of day from `m_cond` (rvk.h `Conditions`, filled by
  `sky.cpp` UpdateConditions) and the weather from `CloudCover`/`StormDeck`/`StormFull`/`ClearSky`. Don't write a new
  "how much is it night" ramp where it is used; add a named one to `Conditions` (and `FL.cond` if shaders need it).
- **Darkness.** Anything that darkens the picture by night, weather or place takes its share of `RVK_Darkness`
  through `m_dark` (`darkness.cpp`), so the one setting keeps controlling it.
- **Plain settings.** A setting that is one value read as it is goes in `RVK_FLOAT_KNOBS`/`RVK_BOOL_KNOBS` (rvk.h),
  not a new setter + ThreadedDevice forwarder + ApplyOne branch.
- **Renames and new defaults.** A renamed setting or a changed default gets a `kMigrations` entry
  (rvk_settings.cpp), or players keep the old value forever (the ini stores every setting). Setting names stay
  within 15 characters, or AOReloaded's window can't show them.

Constraints that matter when touching native code: object layouts and vtables that other game DLLs see
(`RVisual_t` 0x178, `RRefFrame_t` 0xA4, compiled-in allocation sizes, ~120 DisplaySystem subclasses) must stay
binary-identical — the numbers are in `docs/architecture.md` and `interface/*.tsv`. A new port is checked with
`native-check` (computations) and `calllog-ab` (drawing) before its default is switched on, and recorded in
`docs/port-ledger.tsv` / `docs/native.md`.

## Generated files (don't hand-edit)

`*.gen.cpp` / `*.gen.h`: `proxy/exports.gen.cpp` and `interface/` (`tools/gen_interface.py`, which also owns the
hooked exports in the `.def`), `proxy/ddraw/com_trace.gen.h` / `com_stub.gen.h` (`tools/gen_com_trace.py`),
`proxy/native/orig_api.gen.*` (`tools/gen_orig_api.py`), `proxy/native/entry_guard.gen.h`
(`tools/gen_entry_guard.py`), `docs/settings.md` (`tools/gen_settings_doc.py` from the settings table: run it after
adding or changing a setting; `--check` says whether it is current).

## Local-only material

`orig/`, `re/` (decompiles), `ghidra/`, `logs/` and `proxy/native/private/` are gitignored; reverse-engineering
scripts are in `ghidra-scripts/` (run via `tools/ghidra-run.sh`). No Funcom files may be committed.

## Changelog

`CHANGELOG.md` records every change a player would notice: new features, fixes, changed or renamed settings,
new diagnostics. Keep it up to date as work lands.

- **Add the entry in the same commit as the change**, under `## Unreleased`, in the right group:
  `New`, `Changed`, `Fixed`, `Diagnostics`. Create a group if it isn't there yet.
- **Write for players, not developers.** Say what they see and where (zone, window, effect), not the
  function or file that changed. "The PF map's black areas showed the world through them" beats "UI layer
  clear wrote alpha 0". Name new or renamed `RVK_` settings and hotkeys.
- One bullet per change, a sentence or two. Add the GitHub issue as `(#12)` when there is one.
- Purely internal work (refactors, the test harness, profiling tools, port bookkeeping) gets no entry unless
  players notice it.
- On a feature branch, add the entry there too. When merging into main, keep the entries from both sides
  under `## Unreleased`.
- **At release:** rename `## Unreleased` to `## X.Y.Z - YYYY-MM-DD`, start a fresh empty `## Unreleased`
  above it, and draft the GitHub release notes from that section.

## Release builds

Before cutting a release, check `git branch --no-merged main` (and the worktrees): work on feature branches
is often newer than main. Merge what belongs in the release first.

Every release build gets a new tag on GitHub: commit the changelog, tag the commit with an annotated
`vX.Y.Z` tag (message `ao-vk X.Y.Z`), and push the branch and the tag (`git push github main vX.Y.Z`).
The version exists only in the tag and `CHANGELOG.md`; nothing in the DLL carries it. Then:

- Build from a clean directory (`cmake --preset linux-release -B build/release-X.Y.Z`, then
  `cmake --build build/release-X.Y.Z`) and run `tools/native-check.sh`, `tools/rvk-demo.sh`,
  `tools/crash-check.sh`, and `tools/calllog-ab.sh <Mode>` for any `[Native]` switch that changed.
- Everything for a version lives in one folder, `../dist/ao-vk-X.Y.Z/`:
  - `ao-vk-X.Y.Z/`: the zip's contents (the zip's one top-level folder): `randy31.dll`, AOReloaded's
    `version.dll` (built from `../AOReloaded`), `INSTALL.txt`, `README-ao-vk.md`, `LICENSE-ao-vk.txt`,
    `LICENSE-AOReloaded.txt`, `LICENSE-FSR2.txt` (`third_party/fsr2/LICENSE.txt`: AMD's FSR 2 is in the DLL), `LICENSE-FidelityFX-SDK.txt`
    (`third_party/ffx-sdk/LICENSE.txt`: FSR 3.1).
  - `ao-vk-X.Y.Z.zip`: that folder zipped.
  - `release-notes.md`: the GitHub release text, drafted from the version's `CHANGELOG.md` section.
- Create the GitHub release on the tag, named `ao-vk X.Y.Z`, from that folder
  (`gh release create vX.Y.Z ao-vk-X.Y.Z.zip --title "ao-vk X.Y.Z" --notes-file release-notes.md`).

## Conventions

- Comments and docs are plain prose describing behaviour ("what it does, and why"), matching the README's style.
- Commit subjects describe the user-visible effect; issue references as `(ao-vk #N)` (in the changelog just `(#N)`).
- New features get an `RVK_*` setting (and usually a hotkey/console name) so they can be switched off; plans for
  larger features live in `docs/*-plan.md`.
