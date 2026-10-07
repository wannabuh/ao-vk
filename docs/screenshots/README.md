# Screenshots

Before / after images for the project README, made by `tools/screenshot-pairs.py` from pairs of game screenshots.

1. In game, hide the interface windows, find the spot and take a screenshot.
2. Press **Ctrl+Shift+E** (switches every ao-vk enhancement off: the game's own look) and take another without moving
   the camera. Press it again to switch them back on.
3. Put both in a folder as `NAME-before.png` (the game's own look) and `NAME-after.png` (ao-vk). A clip goes in as
   `NAME.gif`.
4. Run `tools/screenshot-pairs.py THAT_FOLDER`: each pair becomes `docs/screenshots/NAME.jpg`, the two side by side
   and labelled (`--stack` puts them one above the other), 1600 pixels wide (`--width`). It prints the markdown for
   the README.

The pairs in use:

| Name | What it shows |
|---|---|
| `field` | A grassy area by day, low sun: the ground grass, sun shadows, HDR, colour grading |
| `night` | A town at night with lamps: per-pixel lighting, lamp shadows, night glow, bloom |
| `grass` | Grass up close at a path's edge, towards the sun: blade shadows, backlit tips, flowers |
| `character` | A character in sunlight: smoother outlines, a real shadow instead of the blob |
| `wind.gif` | Grass in the wind with a character walking through it (no before shot) |
