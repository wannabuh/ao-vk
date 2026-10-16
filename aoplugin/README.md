# ao-vk.dll - ao-vk's AOPlugins plugin

The renderer's settings in the game's options window (F10, **Renderer** tab), for clients running
[AOPlugins](../../AOPlugins) as their `version.dll`. randy31.dll itself knows nothing about AOPlugins: it exports its
settings table (`RvkSettings_*`, `RvkPresets_*` - `proxy/ddraw/rvk_settings.h`) and keeps its settings in
`randy-vk.ini`; this plugin turns that table into the tab.

- every renderer setting the renderer shows (not its hidden or Debug ones: randy-vk.ini only) a DValue bound to a
  widget, changes passed to the renderer, which applies and saves them; the preset buttons follow (Custom after a
  change by hand). Two tabs, as in the AOReloaded fork: **Renderer** (presets, frame rate, view distance, the on / off
  switches, the experimental ones in a block of their own) and **Advanced** (every slider and choice)
- the client's frame rate cap (AFCM.dll's `Timer_t::m_vMaximumFramerate`, stock 100): `AOVK_FpsCap`, kept by AOPlugins
  in `AOPlugins.ini` `[ao-vk]`; the first time it takes over the AOReloaded fork's `AOR_FpsCap`
- view distance (from the fork's AOReloaded tab): N3's 80 m character clamp and the ground's index overflow past 44
  patched, so the game's own sliders go to 300 m (characters) and 150 (ground full quality), the chosen values kept
  (`AOVK_CharDist`, `AOVK_GroundHQ`) and put back after login; object detail distance (`AOVK_ObjDist`, 100-400 % of
  N3's statel rings, from the next zone) in the Renderer tab. Patches go through AOPlugins (checked bytes, one owner);
  a different client build is left unpatched and the log says so
- a lost or timed-out server connection goes back to the login screen, the way a finished /camp does, instead of
  closing the game and starting the launcher (GUI.dll's `ServerLostMessage` / `ServerTimeoutMessage` pass
  `ActivateGameClosing` status 2, "to login", instead of 3 / 4); the disconnect report is no longer sent
- without ao-vk's randy31.dll (the game's own renderer in place) the tab says so and has only the frame rate cap and
  view distance
- an exclusive plugin (a "project" in AOPlugins' terms), id `ao-vk`

It replaces the Renderer tab of the AOReloaded fork's `version.dll`; a GUI skin's static copy of the tab (PRK-VK's
`OptionPanel/Root.xml` has one) is replaced by it in memory.

## Build, test, install

```sh
cmake --preset linux-release && cmake --build --preset linux-release     # in aoplugin/, WSL / Linux: ../build/aoplugin/ao-vk.dll
```

`powershell -ExecutionPolicy Bypass -File aoplugin\test-fake-client.ps1` runs it in AOPlugins' fake client with
ao-vk's real randy31.dll (and once without it); it needs AOPlugins built with its tests.

Install: `ao-vk.dll` into the client's `AOPlugins\` folder (AOPlugins' `version.dll` in the client folder).

`include/aop_plugin.h` is AOPlugins' plugin API header, copied from AOPlugins (API version 2, which this plugin
needs: patches, DValue ranges, the game frame); update it from there.
