#!/usr/bin/env python3
"""Before / after images for the README: each pair of game screenshots becomes one labelled image in
docs/screenshots/, resized and saved as JPEG so the repository stays small.

    tools/screenshot-pairs.py SOURCE_DIR [--out docs/screenshots] [--width 1600] [--stack] [--quality 85]

SOURCE_DIR holds the pairs as NAME-before.EXT and NAME-after.EXT (png, jpg or bmp; take them at the same spot,
toggling ao-vk with Ctrl+Shift+E). Each pair becomes NAME.jpg: the game's own look on the left (or on top with
--stack), ao-vk on the right, each labelled. A file without its partner is skipped with a warning; NAME.gif (a
clip) is copied as it is. At the end the README markdown for the images is printed. Needs Pillow.
"""
import argparse
import pathlib
import shutil
import sys

from PIL import Image, ImageDraw, ImageFont

LABELS = ("Game's own renderer", "ao-vk")
EXTS = (".png", ".jpg", ".jpeg", ".bmp")


def font(size):
    for name in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "arialbd.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def label(img, text):
    """The label in the top-left corner, white on a dark translucent box."""
    draw = ImageDraw.Draw(img, "RGBA")
    f = font(max(14, img.width // 40))
    pad = f.size // 2
    box = draw.textbbox((0, 0), text, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    draw.rectangle((pad, pad, pad + w + 2 * pad, pad + h + 2 * pad), fill=(0, 0, 0, 160))
    draw.text((2 * pad - box[0], 2 * pad - box[1]), text, font=f, fill=(255, 255, 255, 255))


def combine(before, after, width, stack):
    a = Image.open(before).convert("RGB")
    b = Image.open(after).convert("RGB")
    if b.size != a.size:                       # (a different window size: the after shot to the before's)
        print(f"  note: {after.name} is {b.size[0]}x{b.size[1]}, {before.name} {a.size[0]}x{a.size[1]}; resizing")
        b = b.resize(a.size, Image.LANCZOS)
    gap = max(2, width // 400)                 # the dividing line
    half = width if stack else (width - gap) // 2   # each shot's width in the result
    h = round(a.height * half / a.width)
    a = a.resize((half, h), Image.LANCZOS)
    b = b.resize((half, h), Image.LANCZOS)
    label(a, LABELS[0])
    label(b, LABELS[1])
    out = Image.new("RGB", (half, 2 * h + gap) if stack else (2 * half + gap, h), (0, 0, 0))
    out.paste(a, (0, 0))
    out.paste(b, (0, h + gap) if stack else (half + gap, 0))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("--out", type=pathlib.Path,
                   default=pathlib.Path(__file__).resolve().parent.parent / "docs" / "screenshots")
    p.add_argument("--width", type=int, default=1600, help="width of the result in pixels (default 1600)")
    p.add_argument("--stack", action="store_true", help="before above after instead of side by side")
    p.add_argument("--quality", type=int, default=85, help="JPEG quality (default 85)")
    args = p.parse_args()
    if not args.source.is_dir():
        sys.exit(f"{args.source}: not a folder")
    args.out.mkdir(parents=True, exist_ok=True)
    files = {f.name.lower(): f for f in args.source.iterdir() if f.is_file()}
    names = sorted({n.rsplit("-", 1)[0] for n in files
                    if pathlib.Path(n).suffix in EXTS and pathlib.Path(n).stem.rsplit("-", 1)[-1] in ("before", "after")})
    made = []
    for name in names:
        pick = lambda which: next((files[f"{name}-{which}{e}"] for e in EXTS if f"{name}-{which}{e}" in files), None)
        before, after = pick("before"), pick("after")
        if not before or not after:
            print(f"skipping {name}: no {'before' if not before else 'after'} shot")
            continue
        dest = args.out / f"{name}.jpg"
        combine(before, after, args.width, args.stack).save(dest, quality=args.quality, optimize=True, progressive=True)
        print(f"{dest} ({dest.stat().st_size // 1024} KB)")
        made.append(dest.name)
    for n, f in sorted(files.items()):
        if n.endswith(".gif"):
            shutil.copyfile(f, args.out / n)
            print(f"{args.out / n} ({f.stat().st_size // 1024} KB, copied)")
            made.append(n)
    if made:
        print("\nREADME markdown:\n")
        for n in made:
            print(f"![{pathlib.Path(n).stem}](docs/screenshots/{n})\n")


if __name__ == "__main__":
    main()
