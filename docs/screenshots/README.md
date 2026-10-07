# Screenshots

Before / after images for the project README, made by `tools/screenshot-pairs.py` from pairs of game screenshots.

1. In game, press **Ctrl+Shift+H** to hide the interface (again to bring it back), find the spot and take a
   screenshot.
2. Press **Ctrl+Shift+E** (switches every ao-vk enhancement off: the game's own look) and take another without moving
   the camera. Press it again to switch them back on.
3. Put both in a folder as `NAME-before.png` (the game's own look) and `NAME-after.png` (ao-vk). A clip goes in as
   `NAME.gif`.
4. Run `tools/screenshot-pairs.py THAT_FOLDER`: each pair becomes `docs/screenshots/NAME.jpg`, the two side by side
   and labelled (`--stack` puts them one above the other), 1600 pixels wide (`--width`). It prints the markdown for
   the README.

The images in use (the project README's Screenshots section):

| File | What it shows |
|---|---|
| `west-bank.jpg`, `west-bank-gate.jpg` | West Bank by day: sun shadows, HDR, colour grading, ground grass |
| `newland.jpg` | Newland by day: grass and paths |
| `newland-night.jpg`, `borealis-night.jpg`, `night-lamp.jpg` | Night: lamps lighting their surroundings, lamp shadows |
| `depth-of-field.jpg` | Depth of field focused on the character |
| `particles.jpg` | GPU particles with depth of field (a single shot, no pair) |

Videos are not kept here: GitHub plays only videos uploaded through its web editor, so they are uploaded there
(1080p H.264, under 10 MB each) and their links put in the README.
