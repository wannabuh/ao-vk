#!/usr/bin/env python3
"""Dump a character model (.cir) and animation (.ani) from the client's rdb.db as the raw streams randy31's
CATMesh_t / CATKeyframeAnimData_t constructors read (DisplaySystem's wrapper in front stripped), for
tests/randy_harness.cpp --character. Uses ~/projects/ao-mods/ao-assets (AO_ASSETS) for the database and formats.

Usage: tools/extract-character.py <cir id> <ani id> <out dir>      e.g. 5900 9386 build/characters
Writes <out>/<cir id>.catmesh and <out>/<ani id>.catanim, and per material slot of the model its skin texture as
<out>/<cir id>.tex<slot> (u32 width, u32 height, BGRA rows) when it has one.
"""
import os
import pathlib
import struct
import sys

sys.path.insert(0, os.environ.get("AO_ASSETS", str(pathlib.Path.home() / "projects/ao-mods/ao-assets")))
from aoassets import rdb  # noqa: E402
from aoassets.formats.cir import TEXDATA_KEY, Cir  # noqa: E402
from aoassets.textures import image_bytes  # noqa: E402

cir_id, ani_id, out = int(sys.argv[1]), int(sys.argv[2]), pathlib.Path(sys.argv[3])
out.mkdir(parents=True, exist_ok=True)
db = rdb.Rdb()

cir = db.get(rdb.T_CIR, cir_id)
count = max(struct.unpack_from("<I", cir, 32)[0] // TEXDATA_KEY - 1, 0)   # name[32], encoded count, texdata, colour
(out / f"{cir_id}.catmesh").write_bytes(cir[32 + 4 + 44 * count + 4:])

import io  # noqa: E402

from PIL import Image  # noqa: E402

for slot, (name, tex_id, _, _) in enumerate(Cir.parse(cir).texture_slots()):
    for table in (rdb.T_CHAR_TEX, 1010019, 1010020, rdb.T_TEXTURE):
        if db.has(table, tex_id):
            image = Image.open(io.BytesIO(image_bytes(table, db.get(table, tex_id))[0])).convert("RGBA")
            r, g, b, a = image.split()
            raw = Image.merge("RGBA", (b, g, r, a)).tobytes()
            (out / f"{cir_id}.tex{slot}").write_bytes(struct.pack("<II", *image.size) + raw)
            print(f"slot {slot} {name}: texture {table}:{tex_id} {image.size[0]}x{image.size[1]}")
            break

ani = db.get(rdb.T_ANI, ani_id)
events = struct.unpack_from("<i", ani, 32)[0]                           # name[32], events {i32 time, char[32]}
(out / f"{ani_id}.catanim").write_bytes(ani[32 + 4 + 36 * events:])
print(f"wrote {out}/{cir_id}.catmesh ({len(cir)} bytes) and {out}/{ani_id}.catanim")
