---
name: sync-upstream
description: Merge wannabuh's ao-vk main (remote `upstream`) into the current ao-vk branch, then port any Renderer-tab changes from wannabuh's AOReloaded master (c:\projekter\AOReloaded, remote `wannabuh`) into the AOPlugins plugin in aoplugin/. Use when asked to merge/sync/update from upstream, from wannabuh, or "both remotes".
---

# Sync from both wannabuh remotes

Two sources, two steps:

1. **ao-vk**: `upstream` = https://github.com/wannabuh/ao-vk.git, branch `main` → merged into the current branch.
2. **AOReloaded**: `c:\projekter\AOReloaded`, remote `wannabuh`, branch `master` → its Renderer tab
   (`src/core/renderer_settings.cpp`, `fps_cap.cpp`, `view_distance.cpp`) is the reference for our plugin
   `aoplugin/ao_vk_plugin.cpp` (+ `view_distance.cpp`). Not merged, ported by hand.

Last synced (update these at the end): ao-vk `upstream/main` **53b6881**, AOReloaded `wannabuh/master` **b1e4c9d**
(2026-10-16).

## Rules

- Never `git push`. Commits stay local.
- No Co-Authored-By / AI attribution in commit messages.
- The working tree often holds the user's uncommitted work. Don't stash, reset or commit it. Stage only the
  files you resolved or changed. If your changes land in a file that already has uncommitted edits, don't commit
  it; ask the user how to split it.

## Step 1: merge ao-vk upstream/main

```sh
cd /c/projekter/ao-vk
git status --short                       # note the user's dirty files
git fetch upstream
git log --oneline HEAD..upstream/main | wc -l
git diff --name-only HEAD...upstream/main | grep -E '<dirty files>'   # overlap → ask before merging
git merge --no-edit upstream/main
```

Known conflict patterns (from the 2026-10-16 merge):
- **CMakeLists.txt**: take the union. Ours: `proxy/native/ui_layer.cpp` (we renamed upstream's `gui.cpp`),
  `proxy/ddraw/rvk_plugin.cpp`, `include` in the include dirs, `aovk_hooklib` (hooklib.cpp + **vc10.cpp**), and the
  `ui_*` glass shaders (`ui_blur_down/up.frag ui_glass.frag ui_shape.frag ui_image.frag ui_mesh.vert/.frag`).
  Keep `vc10.cpp` out of `randy_native`: it's already in `aovk_hooklib`, which `randy_native` links.
- **proxy/ddraw/rvk_settings.cpp**: union of includes (`aovk_plugin.h` is ours) and of the vanilla list
  (`{"RVK_UiSkin", 0}` is ours).
- **rvk/hdr.cpp** pipeline lists: union (`m_glassDown/Up/glassPipeline` are ours).
- **Crash logging** lives in upstream's `proxy/native/crash.cpp` and reports faults in any module. Our old
  `WatchModule` / `g_watchedExtra` in hook.cpp were dropped. Don't bring them back.
- If upstream renames or moves something we extended, follow upstream and port our part. Check that no file still
  includes a header that was renamed (e.g. `gui.h` vs `ui_layer.h`).

Write multi-hunk resolutions as a Python script in the scratchpad (keeping CRLF/LF as found). Don't write a heredoc
after a `cat >` that has no input: it waits on stdin.

## Step 2: build (WSL)

```sh
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-26.04 -- bash -lc 'cd /mnt/c/projekter/ao-vk && cmake --build --preset linux-release 2>&1 | grep -E "error|FAILED" ; cd aoplugin && cmake --build --preset linux-release 2>&1 | tail -3'
```
(`wsl` without `-d Ubuntu-26.04` has no cmake. If there's no build dir, run `cmake --preset linux-release` first.)
Commit the merge after a clean build: `Merge upstream/main into <branch>`, plus a short note on how the conflicts
were resolved.

## Step 3: port the AOReloaded Renderer tab

```sh
cd /c/projekter/AOReloaded && git fetch wannabuh
git log --oneline <last synced>..wannabuh/master
git diff <last synced> wannabuh/master -- src/core/renderer_settings.cpp src/core/fps_cap.cpp src/hooks/view_distance.cpp src/core/settings.cpp
```
Port behaviour, not code: AOReloaded writes XML into a fixed buffer and sets DValues through GameAPI. Our plugin
goes through the AOPlugins host (`addSetting`, `setSetting`, `addOptionsTab` per tab, `settingChanged`).
How things map today:
- wannabuh's Renderer + Advanced ScrollViews → `WriteRendererTab` / `WriteAdvancedTab`, each its own `addOptionsTab`
- `RendererOnSetDValue` (clamp, skip unchanged, preset → `SyncAll`) → `SettingChanged`
- Keep our title "ao-vk renderer" (theirs says randy-vk) and keep our extras: view distance, camp-on-disconnect.

Also check the renderer side after the merge:
- `RvkSettings_Version` and the `RvkSettingInfo` layout in `proxy/ddraw/rvk_settings.h`/`.cpp`. If they changed,
  update the struct in `ao_vk_plugin.cpp`.
- Setting names longer than 15 characters get a cut-down DValue name (`DName`). Hidden settings (`kHidden`, the Debug
  section) aren't exported at all.
- New presets may change what the tests assume.

## Step 4: test

```powershell
powershell -ExecutionPolicy Bypass -File aoplugin\test-fake-client.ps1
```
Needs AOPlugins (c:\projekter\AOPlugins) built with tests. If a check fails because upstream changed behaviour
(e.g. a setting became hidden), update the test to match and say so; don't weaken the plugin.

## Finish

Update the "Last synced" hashes above. Then report to the user:
- each conflict and how it was resolved, especially anything of ours that was dropped
- build and test results
- what is committed, and what is left uncommitted and why
