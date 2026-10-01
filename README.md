# randy-vk

A replacement for Anarchy Online's renderer, `randy31.dll` (Direct3D 7), with the goal of a native
Vulkan renderer. Client-side only; gameplay code is untouched.

## How it works

The built `randy31.dll` exports exactly the same 771 names and ordinals as the original. Every export
this project does not implement yet is a PE forwarder to the original DLL, renamed `randy31_orig.dll`.
Replacing the renderer is done one export at a time by taking its line out of `proxy/randy31.def` and
defining it in code.

No Funcom files are in this repository.

## Build (Linux, cross-compiled)

Needs clang-cl, lld-link, cmake, ninja and the MSVC CRT + Windows SDK unpacked by
[xwin](https://github.com/Jake-Shadle/xwin) into `~/.xwin` (x86).

    cmake --preset linux-release && cmake --build --preset linux-release

## Layout

- `tools/gen_interface.py`: reads the original DLL and its importers, writes `interface/` and `proxy/randy31.def`
- `interface/exports.tsv`: every export with RVA, section, importing modules, demangled name
- `interface/summary.md`: counts per importer and class
- `tests/forward_check.cpp`: run under Wine from the client folder; checks every export resolves to the original

## Install

Put the stock DLL next to the client as `randy31_orig.dll` and the built one as `randy31.dll`.
Forwarder targets are searched relative to the exe, so both must be in the client folder.
