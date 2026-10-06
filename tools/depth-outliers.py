#!/usr/bin/env python3
"""The scene draws a depth pre-pass can't take, from a frame dump (Ctrl+Shift+F9: logs/rvk-frame-HHMMSS.txt).

Lists the 3D draws that don't test depth, don't write it or compare with something other than LESS / LESSEQUAL,
grouped by visual, lighting, blend, depth state and stage 0, with how many draws and how much screen they cover
(their screen rectangles, clipped to the screen, summed: a rough overdraw upper bound). Biggest first.

    tools/depth-outliers.py logs/rvk-frame-213045.txt [--size 2560x1440] [--kind 3] [--all]

--kind: only this visual kind (3 = static, the "statics" of the GPU profile); --all: every draw, not only the
depth outliers.
"""
import argparse
import re
from collections import defaultdict

KINDS = {0: "unknown", 1: "character", 2: "character part", 3: "static", 4: "terrain", 5: "room", 6: "water",
         7: "sky", 8: "blob shadow", 9: "effect", 10: "other"}
BLENDS = {1: "ZERO", 2: "ONE", 3: "SRCCOLOR", 4: "INVSRCCOLOR", 5: "SRCALPHA", 6: "INVSRCALPHA", 7: "DESTALPHA",
          8: "INVDESTALPHA", 9: "DESTCOLOR", 10: "INVDESTCOLOR", 11: "SRCALPHASAT", 12: "BOTHSRCALPHA",
          13: "BOTHINVSRCALPHA"}
CMPS = {1: "NEVER", 2: "LESS", 3: "EQUAL", 4: "LESSEQUAL", 5: "GREATER", 6: "NOTEQUAL", 7: "GREATEREQUAL", 8: "ALWAYS"}
NUM = r"(-?\d+(?:\.\d+)?)"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--size", default="2560x1440", help="screen size WxH (rectangles are clipped to it)")
    ap.add_argument("--kind", type=int, default=None, help="only this visual kind (3 = static)")
    ap.add_argument("--all", action="store_true", help="every 3D draw, not only the depth outliers")
    args = ap.parse_args()
    width, height = (int(v) for v in args.size.lower().split("x"))
    screen = float(width * height)

    groups = defaultdict(lambda: [0, 0.0])
    total_draws = 0
    with open(args.dump, errors="replace") as f:
        for line in f:
            if not re.match(r"D \d+: prim", line):
                continue
            z = re.search(r"\| z (\d+)/(\d+)/(\d+)", line)
            if not z:
                continue
            ztest, zwrite, zfunc = (int(v) for v in z.groups())
            outlier = not ztest or not zwrite or zfunc not in (2, 4)
            if not outlier and not args.all:
                continue
            visual = re.search(r"\| visual (.*?) \((\d+)\)", line)
            name, kind = (visual.group(1), int(visual.group(2))) if visual else ("-", None)
            if args.kind is not None and kind != args.kind:
                continue
            lit = "lit" if " lit 1" in line else "unlit"
            blend = re.search(r"\| blend (\d+)/(\d+)", line)
            blend = "%s/%s" % (BLENDS.get(int(blend.group(1)), blend.group(1)),
                               BLENDS.get(int(blend.group(2)), blend.group(2))) if blend else "opaque"
            depth = "test %s write %s %s" % ("on" if ztest else "off", "on" if zwrite else "off",
                                             CMPS.get(zfunc, str(zfunc)))
            stage = re.search(r"\| st0 (\S+)", line)
            stage = stage.group(1) if stage else "?"
            area = 0.0
            rect = re.search(r"\| rect " + NUM + "," + NUM + "-" + NUM + "," + NUM, line)
            if rect:
                x0, y0, x1, y1 = (float(v) for v in rect.groups())
                x0, x1 = max(0.0, min(x0, width)), max(0.0, min(x1, width))
                y0, y1 = max(0.0, min(y0, height)), max(0.0, min(y1, height))
                area = (x1 - x0) * (y1 - y0)
            key = (name, KINDS.get(kind, "-") if kind is not None else "-", lit, blend, depth, stage)
            groups[key][0] += 1
            groups[key][1] += area
            total_draws += 1

    rows = sorted(groups.items(), key=lambda kv: -kv[1][1])
    print("%d draws in %d groups; screens = summed screen rectangles / screen (an upper bound on what they shade)"
          % (total_draws, len(rows)))
    print("%7s %6s  %-14s %-5s %-25s %-28s %-10s %s" % ("screens", "draws", "kind", "lit", "blend", "depth", "stage0",
                                                       "visual"))
    for (name, kind, lit, blend, depth, stage), (count, area) in rows[:60]:
        print("%7.2f %6d  %-14s %-5s %-25s %-28s %-10s %s" % (area / screen, count, kind, lit, blend, depth, stage,
                                                              name))


if __name__ == "__main__":
    main()
