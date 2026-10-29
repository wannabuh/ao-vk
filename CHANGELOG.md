# Changelog

What changed in each ao-vk release, newest first. Downloads and install notes: the
[releases page](https://github.com/wannabuh/ao-vk/releases).

## Unreleased

### Fixed
- Leaving a shop could turn the whole world black outdoors, by day or night, with only the sky, the interface and a
  little bloom left; it stayed black until Ctrl+Shift+E or another zone change.

## 0.8.0 - 2026-10-29

### New
- ao-vk's own effects: an effect file in `randy-vk\fx` (in the client folder) can stand in for one of the game's
  effects, drawn with ao-vk's particles - glowing, trailing, sparking - in place of the game's sprites. The first one,
  `nano_bolt.rvkfx`, replaces the bolt most nanos cast (Change Form, the Calia forms and some 700 others): light
  gathering at your hand, a bright bolt with a trail of motes and sparks, and a burst with a shock ring on the target.
  `rage_bolt.rvkfx` does the same in fire for Lesser Controlled Rage's cast (and four other nanos').
  `nano_recharge.rvkfx` replaces a nano recharger's burst: violet and cyan energy swirling in round you.
  ao-assets' workbench has an effect editor for them (Effects): every change shows in the running game within a
  second, and *Play in game* plays the effect on your character without casting. *Record the game's* has the game
  play its own effect on you and record it, and the editor's preview then shows the game's beside yours, in a split
  view or both in one scene (Compare). Each emitter's *Size scale* makes all its particles bigger or smaller at once.
  The console's `fx <template> cast` plays a cast at yourself (`capture` records it).
  Edited files show at once; the console's `fx reload` reads them again. `RVK_Fx` (Particles) switches them off.
- Shops' insides can be seen through their open doors from the street, in 3D, instead of a black doorway. A shop
  shows once you have been in one of its kind: going in records the room, and walking through a door remembers where
  it leads (every Fair Trade of the same building then shows it too, until its own door says otherwise). By day the
  room is a little dimmer than the street (`RVK_PortalDim`). The room's other doors show in it too (a shop seen
  before this needs one more visit for them). `RVK_Portals` (General) switches it off. The records stay
  in `randy-vk\portals` in the client folder.
- And the reverse: from inside a shop, its open door shows the street outside, with the sky behind it. The street
  shows once you have stood at that door outside (the buildings and props in front of it are recorded then).
  `RVK_StreetView` (General) switches it off. The ground in front of the door shows too, as ao-vk's ground draws
  it; a street recorded before this needs another stand at its door to get its ground. The street is lit as it was
  when you walked in (the hour's sun, ambient light and fog), not as when it was recorded (`RVK_StreetNow`, ini only). A shut
  door stays shut: the street no longer shows through its panel, and the open door shows the street, not black,
  with the door itself still there as it swings open. Your own head and light bar no longer appear in the
  street seen from inside (recorded while you stood in its doorway).
  No clouds through the door yet.
- Zone changes no longer freeze on a blank picture: the last view of the zone you leave stays up, slowly drifting
  and dimming, while the next one loads, then fades into the new zone once it has settled, its ground and lighting
  already in place. The interface stays up throughout, and moving the camera ends the wait at once. `RVK_ZoneFade`
  (General) switches it off.
- A character's own light (a lamp or glowing item it wears) can be kept from casting that character's shadow on the
  ground and walls around it: switch off `RVK_OwnShadow` (Shadows, under point light shadows). Everything else still
  casts in that light, the other characters near it included, and lamps still shadow everyone.
- Meshes from the ao-assets workbench can have a texture of their own: a building or prop unwrapped again and painted
  in Blender or Substance Painter shows its own texture (with its normal, roughness / metal and glow maps) instead
  of the shared game textures it used. A mesh you are editing also shows its new shape in the running game within a
  second of each save, before it is applied (live preview, hot reload on). `RVK_LiveMeshes` (Materials, on).
- Texture packs can hold remodelled meshes: a pack folder in `randy-vk\packs\` with a mesh's new shape and its own
  texture shows them without any change to the game's files (ao-assets authored packs install themselves there).
  `randy-vk\packs\order.txt` decides which pack wins where two change the same texture or mesh ("materials" stands
  for `randy-vk\materials`); without it nothing changes. `RVK_LiveMeshes` switches the meshes off.
- The water's surface is now built from the game's own water: rivers, lakes and pools sit exactly where the game puts
  them, at every level, instead of being redrawn from a map that lost some of them (dry riverbeds, water under the
  ground, walls of water) in places it hadn't seen before. The waves calm down towards the shores and the falls, and
  each body of water gets waves for its size. `RVK_WaterMesh` (Water, on) switches back to the older surface.
- Tree crowns look round: `RVK_CrownSky` (Lighting, 0 to 1) lights them as the sky does - their tops brighter,
  their undersides and insides darker - and lets the leaves facing the sun keep their shading instead of all reaching
  the same flat brightness. A tree is about as bright overall as before.
- Sunlight through tree crowns follows their depth: `RVK_CrownDepth` (Lighting, under Sunlight through leaves, 0 to 1).
  With the sun behind a tree, its thin edges and gaps glow and its dense middle stays dark, in the deeper yellow-green
  of light through leaves; leaves under another tree's crown or behind a wall get none.
- AMD's reflections to try: `RVK_SsrMode 1` (Effects, under Reflections) swaps our screen-space reflections for
  AMD's FidelityFX SSSR - glossy surfaces get blurred reflections instead of none, and they are denoised over frames.
  Off by default (0: ours).
- AMD's ambient occlusion to compare with ours: `RVK_AoMode 1` (Effects, under Ambient occlusion) uses FidelityFX
  CACAO. `RVK_Ao` and `RVK_AoRadius` still set its strength and reach.
- Ray-traced ambient occlusion on GPUs with ray tracing (NVIDIA RTX, AMD RX 6000 and later): `RVK_AoMode 2`. It sees
  what the screen can't - the underside of a bridge, the inside of a doorway, walls behind the camera - and reaches
  three times `RVK_AoRadius`. Buildings and the ground occlude; characters and plants don't yet.
- Ray-traced sun shadows on GPUs with ray tracing: `RVK_RtShadow 1` (Shadows). Buildings, walls and rocks cast exact
  shadows that are sharp where they touch the ground and soften with distance, with no shadow-map blockiness, and
  every placed object of the playfield casts, seen or not. Characters cast traced shadows too, and so do cut-outs -
  fences, grilles, trees' and still plants' leaves, through their textures' holes. Swaying plants, canopies' leaves
  and characters' hair still cast from the shadow map, and grass, water and glass still take the map's shadows.
  Ray-traced ambient occlusion (`RVK_AoMode 2`) sees characters and cut-outs as well. Needs the sun's shadows and
  the depth pre-pass on.
- Ray-traced reflections on GPUs with ray tracing: `RVK_RtReflect 1` (Effects, under Reflections). Where the
  screen-space reflections find nothing - what stands off the screen's edge, behind a wall, behind the camera -
  a ray finds it: wet ground and glossy floors reflect cliffs and buildings that were cut off before. Things the
  camera hasn't looked at lately reflect as the sky around them.
- Lamps light the air: `RVK_VolLamps` (Effects, under Volumetric light; 0 = off, up to 2) gives every point and spot
  light a glow in the air round it, thicker in fog banks; a lamp with a shadow cube casts shafts through it - dark
  where a crate or a wall blocks its light.
- Variable rate shading: `RVK_Vrs 1` (Effects) shades flat and fast-moving parts of the scene in 2 x 2 blocks, for
  more frames per second on GPUs that support it (AMD's FidelityFX VRS). `RVK_VrsCutoff` (ini) sets how readily.
  Leaves, fences and grilles are always shaded in full, so their holes stay sharp.
- AMD's indirect light to try: `RVK_GiMode 1` (Effects, under Indirect light) traces bounce light through a model of
  the world's buildings and ground (FidelityFX Brixelizer GI), so light reflects off walls and slopes that are off
  screen or hidden too. `RVK_Gi` still sets its strength. In randy-vk.ini, `RVK_GiDebug` 1 shows that model and 2 the
  light alone; `RVK_GiSky` adds the sky's light to it.
- The settings window (AOReloaded's Renderer and Advanced tabs) says what every setting costs: a tag after each one,
  such as [GPU: high] or [no cost], from GPU time measured at 1920 x 1080. A note under the frame rate cap explains a
  graphics card at 100% (normal while the frame rate is under the cap) and what lowers it. The choices show names
  instead of numbers (ours / AMD CACAO / ray traced, FSR 2.2 / FSR 3.1, off / 16x, ...), and AMD's tuning moved from
  randy-vk.ini into the Advanced tab: `RVK_SssrThick`, `RVK_GiSky` and `RVK_VrsCutoff`. The console shows the cost
  too. Needs the matching AOReloaded.

### Changed
- Open windows (inventory, chat, the map) cost far less of the graphics card: the interface is drawn with a lean
  shader of its own instead of the full scene shader, the same picture (about 1.5 ms less a frame with a few
  windows open on an RX 6600 XT). `RVK_LiteShader` (randy-vk.ini, on) switches it off. Contributed by gorg (#21).
- Day and night share one light (`RVK_LightRig`, on; off brings back the old night lighting). At sunset buildings and
  rocks no longer light up and then drop to a third of their light as the moon rises; ground in a hill's shade no
  longer brightens in the last minutes of daylight, the difference between sun and shade fading out as the sun goes
  down; in a storm at night the ground and what stands on it darken alike; and by night buildings, rocks and
  characters are lit by the same night sky as the ground around them, no longer at about half its brightness. How
  dark the land gets now follows the light of the hour, as eyes adapt: a moonlit night is lighter than a moonless
  one, and the darkness floor's slow brightening is gone (`RVK_Darkness` still sets how dark).
- The land, sky and clouds share one exposure, metered like a camera's: when the view holds much more bright sky
  than the zone's daytime balance (looking into a sunset), the whole picture comes down a little together. It only
  ever darkens, never below 0.4 of the exposure; a zone not yet seen by day is metered against a usual day.
  `RVK_Metering` (Lighting) sets how strongly (0 turns it off).
- Faster on the GPU wherever buildings, characters and plants fill the view: they no longer share the ground's heavy
  shader build, and those no lamp reaches skip the lamps' code as well, so the graphics card works on more of their
  pixels at once (a city's scene by day about half the GPU time).
- The ground draws faster on the GPU too (~14% less of its time): its shader no longer holds the ground's texture
  layout through the lighting, so the graphics card keeps a third more of its pixels in flight.
- Floodlights and other spot lights cost much less for their shadows: only the part of their shadow map their beam
  can reach is drawn, not all six sides with everything in range (Borealis's bridge lights reach over 1,000 units).
  Grass outside their beam no longer goes into their shadows either, and with no swaying grass in the beam their
  shadows are kept between frames instead of redrawn. Their light in the air (`RVK_VolLamps`) skips the work for the
  view's rays the beam doesn't reach, and buildings and the ground outside the beam no longer work out its light.
- Sun shadows cost less: beyond the nearest of the four shadow areas round the camera, they are redrawn in turns -
  the second every other frame, the two widest every fourth - instead of the second every frame and the widest every
  other (about a third less GPU time for sun shadows over a city). Still things' shadows are exactly as before;
  moving the camera far, a new sun angle or a change of shadow settings redraws them all at once.
- The rocks ao-vk scatters at the feet of cliffs cost less: they are now skipped in blocks that are out of view,
  and only the blocks a sun shadow's or a lamp's shadow map covers are drawn into it, instead of a whole stretch of
  land's rocks every time (the rocks you see are the same). Around Borealis's bridges the rocks were most of what the
  nearby sun shadows drew.

### Fixed
- Shadowlands: the void around the floating land (the gardens, the drop below Adonis and other zones) no longer
  shows a black plain tiled with the word "Brink". That is the game's marker for the void, a see-through ground tile;
  the ground drawn by ao-vk (`RVK_GroundOn`) now leaves those cells out as the game does (#18).
- Crashes on zoning or in doorways (Windows: "heap corruption"), more frequent since 0.7.1: the Shadowlands
  special light on characters, and any object drawn with a set of its own textures, could free a texture the game
  had already let go and then free it again (#19).
- At night near shops, bright slanted sheets and white streaks no longer hang over the street. They were the soft
  edges of a shop room's glass and light beams, seen through its doorway (`RVK_Portals`), drawn outside the doorway.
- With frame generation on (`RVK_FrameGen`), AOReloaded's cast and reload bars and the text shown after zoning
  (the suppression field notice) no longer smear: they were drawn into the picture frame generation fills in.
- In storms, water no longer jerks about: the waves follow a slow swell instead of every swing of the storm's wind,
  and the small ripples no longer jump when the wind veers.
- A shop's room no longer shows from behind its doorway, floating in the street as a pale window with its lamp beams
  (Newland City's midtech shop, from the arcade beside it). The room also no longer sticks out round the doorway:
  through a wide doorway its walls showed past the frame, and with the camera in the doorway the screen filled with
  broken triangles.
- A shop's room seen through its doorway is no longer turned at an angle to the building (30 degrees at Newland City's
  midtech shop, whose doorway is a pointed recess rather than a flat panel), and no longer shows over the walls just
  inside the doorway: it shows only where the doorway's black is.
- Forests next to water no longer cost far more than other forests: the trees drawn after a pond or river were
  shaded once for every layer of leaves (about 46 fps under a Newland tree, where a forest without water stays fast).
  `RVK_PrepassMore` (ini only) switches the fix off for comparison.
- Ground grass grows on the grass again, not beside it: in a busy town (the Borealis lawns) blades stood on the
  paving while patches of lawn next to them stayed bare. The grass took the ground's look while its textures were still
  loading and kept it; it now waits for them, and looks again whenever one changes.
- Walking into a shop or other room from outside no longer leaves it dark for its first five seconds (it was lit as
  the street by night until then).
- Your character's own light keeps lighting the walls and floor around you as you step through a door between two
  parts of a room, instead of going out on the side you just left.
- Inside shops and other rooms, the props and characters were much darker than the walls and floor around them.
  Lamp brightness (`RVK_PtLight`) now only applies outdoors: in a room the lamps are its light, at the game's strength.
  A room's walls and floor take only the lights the game gives them (their lamps are already in their baked light), so
  the room isn't washed out, and the lamps' glow in the air (`RVK_VolLamps`) stays at `RVK_PtLight` indoors too, so a
  shop no longer fills with haze.
- A ceiling lamp over a shop terminal or other prop could be taken for a light a character carries, dimmed to
  `RVK_CharLight`, and with `RVK_OwnShadow` off the prop under it cast no shadow.
- At night in open zones, a lit strip ran round the whole horizon just under the sky, with a hard top edge and the
  far hills dark against it: the game's cloud sheets that sit below eye level, far off, showed there. They're no
  longer drawn under the horizon.
- With ray-traced ambient occlusion (`RVK_AoMode` 2), the shading slid a few pixels behind walls and corners while
  strafing, and slid back once the camera stopped.
- By night, grass growing right behind a tree trunk (or anything else solid) showed in front of it. The grass was drawn
  with the camera the game had set last for its night effects, not the world's.
- With depth of field on (`RVK_Dof`), the glow a lamp makes in the air (a character's own light at night, in rain or
  mist) was dimmed by the haze of whatever lay behind it: trees behind your character stood out as dark, sharp-edged
  shapes in its glow, and the sky lost most of it.
- In a thunderstorm at night, buildings, rocks, pillars and characters went near black against the ground around
  them: the storm took three quarters of their light where it takes half of the ground's. By night they now dim as
  the ground does.
- Standing in grass with the camera zoomed out, the grass could stand up through your character instead of bending
  away from it, when other characters were walking nearer the camera (their footsteps took every place the grass
  keeps for what pushes it). Characters where they stand now always come first.
- Static meshes are now recognised in the game: live geometry and own-UV atlases from the ao-assets workbench didn't
  show on them, and Ctrl+Shift+J on a static mesh couldn't name it (it gave its texture and counts instead).
- Texture and mesh picking (Ctrl+Shift+I / Ctrl+Shift+J) picked whatever was up and to the left of the cursor when
  `RVK_RenderScale` is below 100 (75 by default), often an object behind the one pointed at; it now picks what is under
  the cursor.
- Sunlight through leaves (`RVK_LeafLight`) did little or nothing on dense tree crowns: their textures were taken
  for walls drawn the same way. Every tree crown now lets the light through.
- Chain-link fences' sun shadows flickered between the mesh pattern and solid panels while the mouse pointer was
  hidden (after turning the camera with the mouse), on the frames that redrew the interface. Fences, grilles and
  leaves now keep the holes in their shadows every frame.
- Rivers missing in Borealis: the canyons under the bridges showed dry sand where the game has water. The playfield
  keeps a second sheet of water hidden under the land, and the river was put at its height, under the ground.

### Diagnostics
- The frame timing in randy-vk.log (`cpu ms` lines) splits the time spent presenting a frame into acquiring the
  next picture, handing the frame to the GPU and presenting it, so a log of a session that slows down over time
  shows which step grows (#20).
- randy-vk.log lists, at startup, how the graphics driver compiled each version of ao-vk's main shader: the
  registers each needs and what spills to memory (`shader stats:` lines). They show how much of the GPU each kind
  of draw can use at once; please include them when reporting performance on a GPU of your own.
- `RVK_FxLog 1` (randy-vk.ini) writes every effect the game starts to randy-vk.log: its template number and kind,
  what it is attached to (a spot, a character, a pair for a projectile) and when it ends. The console's
  `fx <template>` plays one of the game's effects on your character. The groundwork for ao-vk's own effects.
- Inside a shop, when its open door shows black instead of the street, randy-vk.log now says why (no street
  recorded for that door, the room's entrance not known yet, ...).
- Frame dumps also save AMD's indirect light (`-brixgi.bmp`, with `RVK_GiMode 1`) and the surfaces AMD's effects read
  (`-surface.bmp`: normals, characters' marked twice as bright); `-gi.bmp` stays ours.
- `RVK_RtDebug 1` (randy-vk.ini) shows what ray tracing sees on GPUs that have it: each building, rock and patch of
  ground in its own colour (red: a surface seen from behind). The ray-traced effects are built on it.

## 0.7.1 - 2026-10-26

### Fixed
- Long sessions and many zone changes no longer run the game out of memory and crash it. The game keeps every zone's
  textures, and ao-vk kept a second copy of each in the game's memory as well (over half a gigabyte after a few
  zones, nearly two in a long session; the game is a 32-bit program and has four at most). A texture's copy is now
  dropped once it is on the graphics card. `RVK_TexKeepCpu` (randy-vk.ini) keeps them as before.
- Zoning no longer fills the graphics card's memory: textures the game let go of when leaving a zone (small interface
  and ground pieces, thousands of them after a few zone changes) stayed on the card for the rest of the session.
- A crash in busy zones with many different objects in view: once the renderer's per-frame buffer was outgrown, every
  frame wrote past its end until the game went down (randy-vk.log filled with "bytes do not fit in the ring buffer").
  The buffer now grows for the next frames, and nothing is ever written beyond it.
- With frame generation on (`RVK_FrameGen`), the game could freeze for good on a loading screen when anything drawn
  over the picture appeared or went away there: switching on the performance readout during a zone change or at
  character select did it every time, and the interface coming or going during a load could too.

### New
- The performance readout (`RVK_PerfHud`) shows client memory and VRAM. Client memory is how much of the game's 4 GB it
  has taken, with the MB still free; it turns orange under 512 MB, where long sessions start to fail on zone changes
  (the game is a 32-bit program). VRAM is the graphics card's memory in use against what the driver allows the game.

### Diagnostics
- Crashes that used to leave randy-vk.log simply ending now leave a report: errors thrown in code (out of memory
  during a zone load), crashes nothing caught, and the renderer's own fatal errors. A normal quit ends the log with
  "the game exits", so a log without it was ended from outside the game.
- When the game stops responding (Windows greys its window and offers to close it), randy-vk.log now gets a hang report
  after about ten seconds, even if the picture still updates. Before, only a picture frozen for 20 s got one, and a game
  closed from that Windows prompt left nothing in the log.
- randy-vk.log notes the free memory at each zone change and in every crash report, with the largest free block, and
  every 600 frames what the game's textures are: how many are alive, made and freed, and whether any are held twice.

## 0.7.0 - 2026-10-20

### New
- One setting for how dark the game gets: `RVK_Darkness` (Lighting, first in the list). It sets nights, the night sky
  and clouds, the moon's light and thunderstorms together: 0 is the lightest (nights about as light as the blue hour
  after sunset), 1 is the darkness ao-vk had before, 2 is very dark. It takes the place of `RVK_NightDark`; a value
  you had chosen carries over.
- The darkest places no longer go near black: when the land shows darker than `RVK_Darkness` allows (a night
  thunderstorm, a cave, a dim basement), the picture brightens over a couple of seconds, as eyes adapt to the dark.
  At `RVK_Darkness` 2 it never does. `RVK_DarkFloor` in randy-vk.ini (1; 0 for none) sets how much.
- The ground is drawn by ao-vk itself (`RVK_GroundOn`, on): every patch of land shows its own textures out to the
  horizon instead of the game's blurry baked pictures past a short ring, and walking no longer makes the game rebuild
  the ground (no stutter, less memory). Cliffs, rock, shadows, mist and wet ground work on it as before, and texture
  packs' ground maps too. Turn it off to get the game's own ground back.
- On that ground, valleys, cliff faces and the feet of canyon walls are darker where less of the sky reaches them
  (`RVK_GroundAo`, 0.6; 0 for none). Canyon walls no longer show the dark and green triangles of the ground's
  coarse light and tile colours, and the grass grows on it as before.
- Up close, that ground's hills and the feet and rims of cliffs are rounded instead of made of flat 4 m triangles
  (`RVK_GroundTess`, 0.6; 0 for the game's shapes), and the rock on cliff faces stands out of them in ledges and
  cracks (`RVK_CliffDepth`, 0.4; 0 for flat faces).
- On that ground, hillsides and cliffs take their light from where the sun is now, so slopes brighten and fall into
  shade as it moves (`RVK_GroundSun`, 1; 0 for the game's light as baked for the hour), and when the game moves to the
  next hour's light the land fades into it over a few seconds instead of changing all at once.
- Rocks and boulders lie at the feet of cliffs and canyon walls, and stones on rough slopes, in the colour of the
  rock around them and lit and shadowed like the ground (`RVK_GroundRocks`, 1; 0 for none, up to 2 for twice as
  many). Meadows, roads and towns are left clear.
- Small loose stones lie on flat dirt, gravel and sand near you, grey on gravel and the colour of the ground on dirt,
  each its own shade, and grow out of the ground as you walk towards them instead of popping in (`RVK_Pebbles`,
  1; 0 for none, up to 2 for twice as many). Grass, roads, trails, slabs and concrete stay clear; which ground gets them can
  be set by hand per playfield in `randy-vk\terrain\pebbles.tsv` (playfield, surface type, how many). On desert
  sand only a few lie about, in the sand's own colour.
- The ground at your feet has fine grit in it instead of a soft smear of its texture (`RVK_GroundGrain`, 0.6; 0 for
  none).
- Loose sand in the deserts (Newland Desert, the Perpetual Wastelands, Wailing Wastes...) has wind ripples: their
  crests wander and break off, up close they have real depth with sand settled in the troughs, and in a low sun each
  crest throws its shadow over the next (`RVK_SandRipple`, 1; 0 for none). The wind's larger ripples carry them out to
  the dunes in the distance, which no longer look like smooth felt. The grit of other ground is left off sand.
  How smooth they are is its own setting (`RVK_RippleSoft`, 0.3; 0 for more wobble in their crests and the sand
  texture's own bumps under them, 1 for the smoothest).
- Dunes are shaped by the sun as it stands: their far sides fall into shade, a little cooler, even at noon, where the
  game's light left them almost as bright as the sunny side (`RVK_DuneLight`, 0.6; 0 for the game's flat light).
- Characters leave footprints in the desert sand as they walk, left and right by turns, pressing the ripples flat;
  they soften away over a few minutes (`RVK_SandPrints`, 1; 0 for none).
- In the sun, loose sand near you sparkles here and there where a grain catches the light, changing as you move
  (`RVK_SandGlint`, 0.5; 0 for none, 1 for more and brighter).
- The deserts and wastelands have dusty air in fair weather: the sky turns pale and warm towards the horizon, and far
  dunes and ridges fade into a warm haze instead of a cool blue one (`RVK_DesertAir`, 1; 0 for none). In the midday
  sun the far land near the horizon shimmers in the heat (`RVK_HeatShimmer`, 0.5; 0 for none).
- Texture packs and side-loaded materials can give a texture a height map (`_h.png`): up close its relief is drawn in
  depth, bricks and stones standing out of the surface with shadows between them (`RVK_Parallax`, 1, under normal maps;
  0 for flat). This works on buildings and on ground tiles alike. The sand's ripples have their own depth
  (`RVK_RippleDepth`, 1, up to 3): looking along a dune their crests hide the troughs behind them, out to about 45 units. How finely the
  depth is traced is its own setting (`RVK_DepthSteps`, 1; down to 0.5 for speed, up to 3 for sharper edges when
  looking along the ground).
- Different ground textures side by side (gravel beside dirt, where the game has no transition tile) blend into each
  other along a ragged edge instead of meeting in a hard straight line along the ground's grid (`RVK_GroundBlend`, 1;
  0 for the hard edges). Roads and the game's own transitions stay as they are.
- On the ground drawn by ao-vk, cliffs' rock no longer starts along a row of straight-edged teeth under their rims
  (the ground's triangles); it fades in along a soft, wandering line.
- The rocks at the feet of cliffs are shaded smooth, round over their lumps with a few sharp edges, instead of showing
  their triangles.
- On the ground drawn by ao-vk, characters and rocks no longer float above hillsides: ground you can stand on is the
  game's own shape again (its cells split as the game splits them, not rounded), and only steep cliffs are reshaped.
- The feet and rims of cliffs that run at a slant are smooth slopes instead of a row of sharp triangular teeth.
- Texture packs' material maps for ground tiles (shine, wet look, crevices; `RVK_Pbr`) work on the ground drawn by
  ao-vk too, tile by tile as on the game's own ground.
- Distant ground is less blurry: the pictures the game bakes for the land past its detailed ring get twice the
  detail (`RVK_GroundFar`: 16 texels a cell by default; 8 for the game's, 32 for more at a large memory cost).
- The ground keeps its sharp, detailed texture further out (`RVK_GroundRange`, 1.5x the game's furthest setting by
  default, up to 2.5 - beyond 2 the game stutters as you move; 1 for the game's own). Past the game's limit the land turned into a blurry baked picture a
  short way from you.
- Cliffs and canyon walls show layered rock instead of long vertical streaks: steep faces get a rock texture in the
  colour of the ground around them, with its own relief (`RVK_CliffRock`), instead of the ground's texture stretched
  down them (`RVK_CliffTex`; with `RVK_CliffRock` off, the ground's own texture at its true size), without the same
  picture repeating in rows (`RVK_TileHide`). Large areas, cliffs included, get soft lighter and darker patches
  instead of one flat colour (`RVK_GroundVary`, 0 to 2). All three are on by default and are in the new
  Terrain section of the options; flat ground keeps the game's own tile layout.
- You see further: the game's view distance x1.5 (`RVK_ViewRange`, 1 for the game's own, up to 3), and distant land
  and mountains are kept and fogged by their true distance. Before, the game cut them off along a flat plane in front
  of the camera, so far mountains popped in and out as you turned.
- Rocks and shrubs are shown further out (`RVK_DetailRange`, 2x the game's by default; 1 for the game's own).
  They used to vanish about 150 m away while their shadows stayed on the ground.
- AI missions' PF map shows its rooms black on a green background (`RVK_AlienMap`, on by default). The map
  had become black on black, so you couldn't tell which room you were in.
- Dawn and dusk skies: a warm glow round the sun and along the horizon under it, a cooler sky overhead and a pink
  band opposite the sun around sunrise and sunset, in place of the game's one flat colour. `RVK_Twilight` sets how far
  these colours replace the game's sky (0.5 by default, 0 for the game's), `RVK_SkyGlow` the glow (1; 0 for none).
  Clouds and rain mute them. The distant land, the haze, the valley mist and the clouds low in the sky take on the
  same colours, so the hills fade into the sky behind them.
- Our own sky in place of the game's (`RVK_ProcSky`, on): a physical atmosphere that follows the sun - a blue day
  sky, gold then red and pink at sunset with the pink band and the earth's shadow opposite, a violet and then cobalt
  blue hour, a dark night under the game's stars and moons. The game's bright zone colours no longer fight the
  sunset; `RVK_SkyZone` keeps a hint of each zone's colour by day (0.25; 0 none, 1 mostly the zone's), `RVK_SkyBright`
  sets its brightness and the far haze's (1). Overcast and storms grey it. The distant land, the haze and the clouds
  fade into its horizon, the clouds and the shade take the colour of the sky over them, and a low sun's light is
  coloured by the air it passes through.
- Vivid sunsets and sunrises: the horizon under the sun goes gold, then orange and red, with pink above it and a
  violet sky overhead; after the sun has set, a red line on the horizon under a pink-purple arch. `RVK_Twilight` now
  defaults to 0.85 and goes up to 1.5 for stronger colours. Only an overcast, storms and rain mute them now, not a
  few clouds.
- Blue hour: after sunset (and before sunrise) the sky deepens to blue and indigo before the night's sky takes over,
  and the picture turns cooler with it (`RVK_BlueHour`, 1; 0 for none).
- Clouds take the sunset's colours: gold and orange, then red and pink, lit from below for a while after the sun has
  gone from the land, then violet (`RVK_Afterglow`, 1; 0 for the game's hues, 2 stronger). The land's shade takes the
  sky's pink-violet tint at the same time, so the land and the sky match.
- Stars come out slowly as the sky darkens after sunset, and fade before sunrise (`RVK_StarFade`, on).
- Sunset rays: with the sun low, beams of its light between the hills and through the gaps in the clouds
  (`RVK_DuskRays` under volumetric light, 1; 0 for none).
- Morning haze: the air is thicker round sunrise, and a little round sunset (`RVK_MornHaze`, 1; 0 for none).
- Low sun light: round sunrise and sunset the land, grass, water and everything the sun reaches turn golden to
  orange, and the shade a little cooler, instead of the game's grey light (`RVK_DawnLight`, 1; 0 for the game's
  colours, 2 stronger). Cloud and storms mute it.
- The sun's disc turns golden below about 25 degrees and orange near the horizon, instead of staying white-hot.
- Golden-hour colour grade round sunrise and sunset: warmer light, slightly cooler shade, a touch more contrast
  (`RVK_GoldenHour` under Colour grading, 0.5; 0 for none).
- Thin high wisps of cloud, most on clear days, that catch the light round sunrise and sunset (`RVK_Cirrus`, off by
  default: under the new sky they showed as coarse streaks; 1 to try them).
- The sky's colours can be changed: nine colours in `randy-vk.ini`, as `RRGGBB` like a colour picker gives them, for
  overhead and low by day, at dawn and dusk (towards the sun, away from it, overhead), in the blue hour and at night
  (`RVK_SkyDayTop`, `RVK_SkyDayLow`, `RVK_SkyDuskSun`, `RVK_SkyDuskAway`, `RVK_SkyDuskTop`, `RVK_SkyBlueTop`,
  `RVK_SkyBlueLow`, `RVK_SkyNightTop`, `RVK_SkyNightLow`). The sky keeps its sunsets, glow and smooth changes
  between them, and the haze, clouds and land take on the new colours. They can also be set in the console
  (`skynighttop 05081a`) and take effect at once.
- Our own night sky in place of the game's few hundred single-pixel stars (`RVK_Stars`, 1; 0 for the game's):
  thousands of stars, from faint to glaring, white, gold, red and blue-white, turning slowly through the night with
  the sun's hour, twinkling most near the horizon (`RVK_Twinkle`), dimmer through the thick air low down and hidden by
  clouds and the moon's glow. The galaxy's band crosses it, with dark dust lanes and a brighter core (`RVK_Galaxy`,
  1; 0 for none). `RVK_StarBright` sets how bright they are.
- Shooting stars streak across clear night skies now and then, each its own: most quick, thin and faint, a few long,
  now and then a bright fireball; their glowing trail fades away behind them (`RVK_Meteors`, 1; 0 for none, 3 for
  more).
- Auroras: on some clear nights curtains of green light with violet tops ripple low in the sky towards the north
  (`RVK_Aurora`, 1; 0 for none, 2 for more and brighter; `RVK_AuroraCol` for its colour). `RVK_AuroraForce` shows one
  every night, at that strength (0, the default: only some nights).
- Rainbows: in the minutes after rain, with the sun out and low behind you, a rainbow stands in the sky and over the
  clouds, a fainter second bow outside it with its colours reversed (`RVK_Rainbow`, 1; 0 for none).
- Sky colour presets in the console: `skypreset` lists them, `skypreset rubika` (teal days, violet dusks),
  `alien` (magenta dusks, a cyan blue hour), `noir` (colour drained, deep nights) or `earth` (back to the default)
  sets all the sky's colours at once.
- Heat lightning: on some warm, dry nights a storm too far off to hear flickers in the clouds over the horizon -
  silent flashes lighting up distant cloud in one part of the sky, in bursts (`RVK_HeatFlash`, 1; 0 for none).
- Fog banks: at dawn and after rain the valley mist gathers into thick banks drifting through the low ground, with
  clear ground between, deeper the wetter it has been and most in wet country (`RVK_FogBanks`, 1; 0 for none).

- Render scale (`RVK_RenderScale`, Anti-aliasing, under TAA; console `renderscale`): the 3D world can be drawn at
  85, 75, 67 or 50% of the screen's size and brought back up to full size by the TAA, for a higher frame rate. At 75%
  the world costs the GPU about a third less (at a lamp-lit outpost by night, about 102 fps went to about 125); the
  interface and name labels stay sharp at full size, and loading screens are drawn at full size. It needs TAA on;
  100 (the default) draws everything at full size as before, 75 is a good balance.
- AMD FSR 2.2 and FSR 3.1 as upscalers for the render scale (`RVK_Upscaler`, Anti-aliasing; console `upscaler 1` for
  FSR 2.2, `upscaler 2` for FSR 3.1): a sharper picture at the same scale than ao-vk's own upscaler, most of all at
  50%; FSR 3.1 keeps rain best - at a little more GPU time. ao-vk's own (0) by default.
- Frame generation (`RVK_FrameGen`, Anti-aliasing, under TAA; console `framegen 1`): AMD FSR 3.1 puts a frame of its
  own between each two the game draws, for about twice the frames on screen, at the cost of a little more delay
  between your input and the picture. The interface, the names over characters, the mouse pointer, the console and
  the performance readout are laid over every frame as they are, so text doesn't smear or wobble (the names keep the drawn frames'
  places). Screenshots show them as before. It works with any `RVK_Upscaler` and render scale. Off by default.
### Changed
- The game's own frame work is lighter in busy places: ao-vk checked memory with a slow system call for nearly every
  object drawn. At a lamp-lit outpost by night the frame rate went from about 125 to about 140 fps.
- A little less GPU work for the ground's colour patches (where the ground has material maps) and for smoke that is
  far away or small on the screen, which is simulated less often.
- The ground drawn by ao-vk costs less: it is drawn once instead of in the game's two passes (its texture, then its
  light), with the same look - about 9% of the ground's cost, 0.3-0.4 ms a frame at 1440p where it fills the view.
  `RVK_GroundOne` in randy-vk.ini (1; 0 for the two passes).
- Trees cost less up close: every tree's leaves now go into the depth pre-pass, so a crown many leaves deep is lit
  about once per pixel instead of once per layer (only trees with ao-vk's own leaf art did before). In a demo forest
  the leaves cost about 40% less; their look is unchanged.
- Faster where water or smoke is near but not in view: water out of the camera's view is no longer drawn (about
  1.5 ms a frame with a lake behind you), and smoke out of view - or hidden behind the houses in front of it - is simulated a sixth as often, still
  evolving, still there when you turn round (about half a millisecond with a few chimneys out of sight).
- Trees, bushes and other plants cost much less to draw: the parts of their leaf cards you see through are now
  skipped before they are shaded, where before each was shaded and then thrown away. In a demo forest the leaves cost
  half as much and the frame about a third less; the look is unchanged. `RVK_PrepassEq` in randy-vk.ini (1; 0 for
  the old way).
- Trees and bushes further than 20 m away cost less. Their leaves are drawn in one pass instead of two: the soft edge
  of each leaf is a fine dither that the anti-aliasing (TAA) smooths back out, so a crown no longer gets lit once for
  every layer of leaf edges in it. Closer plants keep the blended edge. In a demo forest the frame cost 10-20% less.
  `RVK_FolDither` (Plants) sets the distance: 0 never dithers, 1 dithers every plant (cheapest). It has no effect
  while TAA is off.
- Nights are lighter by default (`RVK_Darkness` 0): the land and the night sky keep the blue hour's light, the moon
  lights a little more, and storms are less dark. Rooms and other places without a sun no longer get the night's
  darkening on top of their own dim light. Set `RVK_Darkness` to 1 for the old look.
- New defaults, and so a new Modern preset (Classic+ and Ultra build on it): bloom from a lower brightness
  (`RVK_BloomThr` 0.5), softer indirect light (`RVK_Gi` 1) and emissive glow (`RVK_PbrGlow` 1), fainter screen-space
  reflections (`RVK_Ssr` 0.25), generated bump on (`RVK_BumpOn`), motion blur off (`RVK_MBlurOn`), finer clouds
  (`RVK_CloudQual` 3), gentler sunsets and sun glow (`RVK_Twilight` 0.5, `RVK_SkyGlow` 0.25), brighter stars that
  twinkle less (`RVK_StarBright` 3, `RVK_Twinkle` 0.2), and on the ground more grit, rounding, pebbles and ripple
  depth (`RVK_GroundGrain` 1, `RVK_GroundTess` 1, `RVK_Pebbles` 2, `RVK_RippleDepth` 2), the dunes' light at full
  (`RVK_DuneLight` 1), shallower height maps (`RVK_Parallax` 0.3) and the game's detail range for the ground's
  textures (`RVK_GroundRange` 1). Settings you changed yourself stay as you set them.
- Thunderstorm clouds are no longer one dark, shapeless mass: heavy masses and thinner stretches show between them,
  light comes through the thinner parts, their undersides are lumpy and their edges ragged. `RVK_StormThick` (1; up to
  2 for the old, nearly black storm) sets how thick and dark they are.

### Fixed
- Kyr'Ozch aliens came out blown-out white with the enhancements on, worst up close with a light of your own: the
  shine the alien levels lay over every surface was lit by ao-vk's lamps and bloomed like a spell effect. It is now
  the faint sheen the game draws, and parts their texture marks as self-lit brighten only up to its own colour.
- The alien ships' green fog (Kyr'Ozch missions) and other indoor zones' fog are back with the enhancements on: their
  fog took the outdoor sky's colours (our sky, its twilight, the land haze), which washed the green out.
- Doors and other see-through surfaces in alien missions no longer turn into a speckled screen pattern from a
  distance: only plants' leaf edges are dithered far off (`RVK_FolDither`), not surfaces translucent all over.
- Shiny characters up close (the Kyr'Ozch's armour) showed their shine as flickering speckle: the shine layer is
  now rounded like the body under it (`RVK_Tess`), so it covers it evenly again.
- Signs, screens and windows that the game lights itself no longer glow two or three times over when a texture pack
  gives them an emissive map (a clan teleporter's sign came out a blown-out cyan block, its letters lost): the map now
  only says where such a surface glows at night, and `RVK_NightGlow` how much. On other surfaces emissive maps add
  their light as before (`RVK_PbrGlow`).
- The volumetric clouds no longer stand as a ring of heavy cloud round the horizon in every sky: clouds far off are
  as broken up as the ones overhead.
- Dark and coloured blobs on floors round tables, plants and characters (indoors most), flickering and changing as
  the interface updated, are gone: a few broken pixels in normal-mapped floors were spread into blobs by the indirect
  light and the anti-aliasing. (Also: the soft edges of plants and cut-out textures, `RVK_FolEdges`, could be drawn
  with another object's place and textures.)
- The console's `off` for a strength (`gi off`, `ptshadows off`) now switches its effect off instead of setting it to
  its lowest value.
- The valley mist no longer fills rooms, subways and other indoor places at night and dawn.
- Under a thunderstorm, buildings, rocks and walls no longer stand out light against the dark land and sky: the storm
  takes nearly all of the sun's direct light and most of the sky's, and they keep about as much of their light as the
  ground around them, soft, from all round.
- The sand's glints no longer sparkle under a storm or thick cloud: they shine only as much as the sun gets through.
- With `RVK_LightOver` on, lamps and characters' lights cost much less in busy places (Newland City has some 60):
  each spot of ground or wall now works out only the lights that reach it, not every light near the whole surface.
- In cities, rocks and boulders no longer lie on paving or against shop fronts (Borealis): ramps in the ground under
  buildings and steps were taken for cliffs.
- Paved city ground of a cool grey (Borealis's square) is no longer taken for dirt and strewn with pebbles.
- Ground textures from a texture pack (with their own normal map) no longer get the fine rock grit of
  `RVK_GroundGrain` laid over them: it showed as faint scratches that didn't follow a texture drawn in depth.
- Texture picking (Ctrl+Shift+I) on the ground drawn by ao-vk picks the ground texture under the mouse and flashes
  just its tiles, instead of lighting up the whole ground and naming nothing the workbench could open.
- With the ground drawn by ao-vk and a low sun, shadows (a rock's, your character's) no longer stop dead with a hard
  edge where the ground turns away from the sun, leaving lit sand inside them: ground facing away from the sun is in
  shade, and stays so as the sun sets (also in its last few degrees, where they had turned a pale grey as bright as
  the sunlit sand); sand ripples in a shadow no longer look sunlit.
- At sunset, shadows near you keep their full strength until the sun is about 5 degrees up (a rock's long shadow over
  the dunes); only distant ones fade out early, as before, so they don't lie as dark blocks on far mountains.
- Sand no longer glints inside shadows.
- At a low sun, the sand's ripples no longer lose their shadows in square, washed-out patches.
- In a thunderstorm at night the ground no longer stays lit while the characters on it go nearly black: the storm
  dims the ground's share of the sky's light as it does everyone else's.
- The rocks at the feet of cliffs are shaded round by the sun, lit on the side it shines on and darker away from it,
  instead of flat patches of bright and dark triangles (bright undersides at some hours, hard-edged facets in the
  evening and in the shade of cliffs and walls).
- With the camera just below the top of a wall or building (or beside a cliff face), the character and most of the
  world could vanish from view; they stay drawn now.
- The rounded shapes of the ground and of characters (`RVK_GroundTess`, `RVK_Tess`) change smoothly as the camera moves
  instead of popping in visible steps.
- Day turned to night all at once: the picture darkened and took on its night colours within a second, the land's
  light switched to the moon's with its shadows at full strength, and the hills' shade and the low sun's warm tint
  went out together. The change from dusk to night now follows the sun as it sinks, over several minutes of game time.
- Under a storm the land brightened the moment night fell.
- Replacement textures from `materials\` or texture packs whose alpha channel isn't see-through-ness (the bank's
  front plate, where it is a glow mask) showed black where that alpha was 0.
- Free camera (Ctrl+Shift+C): things at the edges of the screen vanished when looking a different way from the game's
  camera, and hills hid what was behind them even when flying high above.
- The ground and its grass lit up and darkened as you turned the camera: the sun was added on top of the ground's
  baked-in sunlight whenever only a few lamps were about. They now stay at the darker, intended level. Without pixel
  lighting this was the case everywhere; with it, the grass still had it.
- Ground grass stayed bright under a thunderstorm that darkened the land around it.
- Ground grass grew up through tree trunks, roots and rocks. It now stops at their base.
- Ambient occlusion showed through fog as dark smudges in corners, most visibly in AI missions' thick fog. It
  now fades with the fog.
- Distant land and valley mist glowed white towards a sun hidden behind thick cloud or a storm. The haze's
  glow towards the sun now dims with the light the clouds let through.
- Under a thunderstorm the land stayed lit as if under a clear sky, and the distance, trees, the fog over the
  water and the rain mist stood pale under the dark green sky (distant trees paler still than the ground under
  them). The sky's light on the ground, grass and everything else now dims with the storm too, and the air fades the
  far land into the storm's own sky. `RVK_StormDark` sets how much.
- The suns still showed as a wide bright halo through dense cloud and storms. Their glow over the clouds
  (`RVK_SunBleed`) now fades with the light the clouds let through, gone under a full cloud deck.

### Diagnostics
- Debug view 11 (`pbrdebug 11`): how many lamps the lighting tests at each pixel (red) and how many of them reach
  it (green).
- A small performance readout in the top left (`RVK_PerfHud`, General; console `perfhud 1`): frames a second, and
  how long the game's thread, the renderer's thread and the GPU each take a frame - the one holding the frame back
  shown in amber. Next to the GPU's time, how busy ao-vk keeps the GPU (its share of each second; other programs'
  GPU work isn't counted), and a CPU line: how busy the whole machine's processor is, and the game's own share of
  it. With frame generation on, a last line gives the frames shown a second, the generated ones included.
- `RVK_SceneProf` (randy-vk.ini or console `sceneprof 1`): the `gpu ms` log line splits the scene by kind of draw
  (buildings, ground, plants, characters, effects, water...) and a `scene shading` line says how many times each
  screen pixel was shaded by each - where a slow spot's time goes. Was the `RANDYVK_SCENE_PROFILE` variable.
- The profiling sweep (Ctrl+Shift+O, console `profile`) measures the newer features too: ground grass, tree leaves,
  water, clouds, weather, smoke, our sky, the native ground and its rocks, pebbles and ripples, depth maps,
  tessellation, normal maps, PBR and albedo maps. It now takes about six minutes.
- randy-vk.log names a setting written twice in randy-vk.ini's `[Renderer]` (after a hand edit): only the first one
  counts, so changing the other did nothing.
- randy-vk.log has a `darkness:` line every 10 seconds: the `RVK_Darkness` setting, how much it is night, how bright
  the land shows against the floor and how much the picture is brightened.
- A frame dump also saves the renderer's in-between images next to it (`-scene`, `-local`, `-albedo`, `-glow`, `-ao`,
  `-gi`, `-volume`, `-contact`, `-env`, `-ssr`.bmp), so a fault can be traced to the step that makes it.
- Console `bisect`: finds which setting causes a fault that `enhance off` takes away. Each step puts half of the
  remaining suspects at the game's value (nothing saved); answer `bisect gone` or `bisect still` - about seven steps.
  `bisect stop` puts everything back.
- `RVK_HeapCheck` (in `randy-vk.ini` only): checks the game's memory for corruption at points each frame and logs the
  first place it is found broken - to track down a crash in the ground grass (1 cheap, 2 thorough and slow).
- With the sun near the horizon, `randy-vk.log` gets a `dusk:` line every couple of seconds (the game's light, ambient,
  fog, sky dome and stars, and ours), and a `time:` line with the hour while `RVK_TimeOfDay` / `RVK_TimeSpeed` run
  the clock: watch a sunset go by and see what changes when.

## 0.6.1 - 2026-10-18

### New
- **In-game console** for the renderer's settings: press the key left of 1. Change any setting by name
  (applied at once and saved), with completion and history; `find`, `list`, `changed`, `reset`, `try` and
  `preset` commands, and the hotkeys' actions.
- **Neon look:** coloured light (signs, screens, coloured lamps) keeps its colour instead of washing out to
  white (`RVK_NeonSat`), and bloom has a tight core with a wide haze, wider in rain and storms at night
  (`RVK_BloomWide`). `RVK_NeonNight` adds an optional night colour grade: teal shadows, richer lights.
- Ctrl+Shift+J on a dungeon's walls, floors or pillars now names the tile piece they are built from, so the
  ao-assets workbench can open it for editing (an edit changes that piece in every dungeon room using it).
- Texture packs: put a pack's maps in `randy-vk\packs\<name>\`. They are used for any texture `materials\`
  has nothing for. Packs are found at startup and listed in the log.

### Fixed
- Squares around glowing effects such as the Temple of the Three Winds portal: their see-through corners were
  shaded as if solid. (#15)
- Glowing effects (the Temple of the Three Winds portal, for one) could be seen glowing through walls and rocks in
  front of them.
- Floating damage and XP numbers (and other text over the world) were dimmed by smoke and changed by fog, bloom
  and colour grading. They are now drawn over the finished picture, still hidden behind walls.
- Light beams and glows on lamps (under the billboards in the cities, for example) swayed like trees and grew
  leaves.
- Grass on steep hillsides stood in visible rows, and grass grew in front of walls at the foot of grassy banks.
- Waterfalls pouring from pipes (the subway and other narrow streams) showed as a wide dark wall of water,
  and the streams themselves were missing.
- The PF map's black areas showed the world through them when the interface refresh rate (`RVK_UiRate`)
  was on.
- Held weapons swayed and bent like plants, most visibly while sitting. (#12)
- AOReloaded (`version.dll`) not loading when compatibility settings were set on `AnarchyOnline.exe`.
  The renderer now loads the `version.dll` beside it if Windows skipped it. (#7)
- Valley mist ended in a hard line against the sky at the hilltops. (#14)
- A rare crash when another thread ran code while the renderer patched the game's function tables.

### Diagnostics
- `[Native] Pick=off|on|verify` in `randy-vk.ini` for problems clicking on characters. `off` uses the
  game's own picking, `verify` runs both and logs where they disagree. (#16)
- Frame captures (Ctrl+Shift+F9) show which character each drawn object belongs to.
- Frame captures also save the water's mesh beside the capture (`...-water.bin`) and list each waterfall and
  how it was drawn.

## 0.6.0 - 2026-10-16

Weather and storms. Clouds, weather, smoke, water and leaves are out of Experimental and on by default.

### New
- **Thunderstorms:** dark storm cloud out to the horizon, the land darkening under it, the game's
  yellow-green storm cast, grey rain veils under the far clouds.
- **Rain** that falls as water: slants with the wind, gusts in curtains, kept off by roofs and canopies,
  stops indoors.
- **Lightning:** each of the game's strikes gets its own branching bolt, lighting the land and the clouds.
- **Sand and ash storms:** a lit, volumetric dust fog that parts round obstacles and thins in the lee of
  hills and buildings; the sun dim and red behind it.
- **Storm wind:** grass, plants, trees, smoke, water, rain and clouds lean and gust together; storms blow
  leaves, straw and twigs across the ground.
- **Clouds that change:** they churn and change shape as they drift; storm cloud boils and rolls.
- **Trees:** canopies shaded as one rounded mass, each tree keeping its own colour.
- **Presets** rebuilt: Ultra, Modern (a step lighter), Classic+ and Original.
- Settings: `RVK_CloudQual`, `RVK_CloudMorph` (Advanced tab); `RVK_RainVeils`, `RVK_StormForce`,
  `RVK_SandForce`, `RVK_StrikeRate` (`randy-vk.ini`).
- Settings window: fewer, plainer options, with sections for clouds, weather, smoke, water and leaves.

### Fixed
- Colour banding in smooth skies; stripes and noise in clouds; lightning making clouds noisy.
- Names over characters drawn flat, after the effects (no blur, fog or bloom).
- Rain and puddles seeming to follow a running character.
- The game's storm sheet and lightning glow making the sky flicker.

### Diagnostics
- Free camera: Ctrl+Shift+C (mouse and WASD).
- Ctrl+Shift+Alt+F9 captures the next 60 frames.

## 0.5.1 - 2026-10-14

### Fixed
- Names drawn as black bars and objects with missing textures after zone changes: room for 65,536 textures
  instead of 4,096.

### Diagnostics
- The periodic log line reports how many textures are loaded; frame captures show each texture's state.

## 0.5.0 - 2026-10-14

### New
- **Sky, air and night:** height haze, sun glow, light rays through clouds, valley mist at night and dawn,
  redrawn suns, dawn and dusk in the game's colours, moonlit nights with moon shadows (`RVK_NightDark`),
  city ships fading into the haze.
- **Clouds, rain and smoke** (Experimental): volumetric clouds following the game's weather with cloud
  shadows; redrawn rain with splashes and rings; smoke as a simulated fluid.
- **Waterfalls** (Experimental): falling sheets with spray, mist and foam.
- **Shadows:** things behind you keep casting; ground grass casts shadows from lamps and character lights;
  shadows fade as the sun nears the horizon.
- **Anti-aliasing:** TAA keeps particles bright; FXAA (`RVK_Fxaa`) for when TAA is off.
- Options (F10): separate **Renderer** and **Advanced** tabs.
- Faster: batched drawing (West Bank city ~95 to 139 FPS on the test machine), quicker planet map reopen,
  cheaper interface text, no hitch every 60 seconds, a pipeline cache (`randy-vk.cache`) for faster starts.
- `[Native]` parts on by default.

### Changed
- `RVK_SmokeDensity` is now `RVK_SmokeThick` (value carried over); `RVK_ShadowMem` is on/off.
- Debug switches (`RVK_PbrDebug`, `RVK_ShadowView`, `RVK_Occlusion`, `RVK_TimeOfDay`, `RVK_TimeSpeed`) are
  `randy-vk.ini` only.
- A non-number value in `randy-vk.ini` falls back to the default.

### Fixed
- Several waterfalls at once (e.g. Broken Shores) froze the game.
- Shadow casters flickering on walls.

### Diagnostics
- Crash and hang reports in `randy-vk.log`; slow frames (over 50 ms) logged with the time of day; two
  clients in one folder get separate logs.

## 0.4.1 - 2026-10-11

### Fixed
- The game not starting on some machines with `version.dll` installed: AOReloaded now patches only the
  game's own modules.

## 0.4.0 - 2026-10-11

### New
- **Swimmable water** (Experimental): a simulated surface with bow waves, wakes, splashes and rings.
- **Leaves** on trees, shrubs and palms (Experimental): fluttering, casting shadows, falling in gusts.
- Side-loaded PBR materials with an environment probe (`docs/materials.md`); picking hotkeys for modders
  (Ctrl+Shift+I texture, Ctrl+Shift+J mesh).
- Renderer tab: Experimental section, larger fonts, view distance settings.

### Fixed
- "Mountains of water": each body of water is flat at its own level.
- Falling leaves landing back in the crown.
- PBR sun highlight on objects in shade.

### Diagnostics
- Pop-in log and a GPU memory report in frame dumps.

## 0.3.1 - 2026-10-09

### Fixed
- Walls of water in the Shadowlands; a small pool flooding Jobe harbour.
- Grass vanishing behind characters; Lush Fields turning flat green after an AI mission; clearer grass
  trails.
- Timer bars and screen sprites faded or blurred.
- The planet map (PF map) drawn black.
- Signs, bridge ropes and wide props swaying.
- Objects displaced when drawn before the frame's lighting was set.
- Very large textures freezing the game; memory buffers staying at their burst size.

## 0.3.0 - 2026-10-07

### New
- **Water:** waves, ripples, refraction, depth colour, caustics, reflections, shore foam; a slider from
  "the game's water made richer" to fully realistic.
- **Presets:** Original, Classic+, Modern, Ultra.
- New defaults (Modern): darker shadows, ground grass on, lighter on video memory.

### Fixed
- Grass growing through platforms and floors.
- Grass and water show "needs HDR" in the options.

## 0.2.0 - 2026-10-07

### New
- **Ground grass** (`RVK_GrassOn`): wind, gusts, sun shadows, trails where characters walk.
- Faster: static meshes kept on the GPU, batched draws, `RVK_UiRate` for the interface, a depth pre-pass
  for cut-outs.
- Soft leaf edges (`RVK_FolEdges`); steadier anti-aliasing.
- `randy-vk.ini` edits apply while the game runs; Ctrl+Shift+H hides the interface.

### Fixed
- Sandstorm tint throwing a dark, hard-edged shadow over the foreground.
- Shadows on and from the sky.
