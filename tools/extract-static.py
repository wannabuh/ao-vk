#!/usr/bin/env python3
"""Dump a static mesh (.abiff, rdb_1010001) from the client's rdb.db as the ObjectArchive stream DisplaySystem
hands to serialize.dll (the wrapper in front stripped), for tests/randy_harness.cpp --static. Uses ~/projects/ao-mods/ao-assets.

Usage: tools/extract-static.py <abiff id> <out dir>        e.g. 17879 build/statics   -> <out>/<id>.archive
"""
import os
import pathlib
import struct
import sys

sys.path.insert(0, os.environ.get("AO_ASSETS", str(pathlib.Path.home() / "projects/ao-mods/ao-assets")))
from aoassets import rdb  # noqa: E402

mesh_id, out = int(sys.argv[1]), pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
data = rdb.Rdb().get(rdb.T_ABIFF, mesh_id)
version, = struct.unpack_from("<I", data, 0)
off = 4
if version == 1:
    n, = struct.unpack_from("<I", data, off)
    off += 4 + max(n // 0x3F1 - 1, 0) * 0x2C
elif version == 3:
    count, = struct.unpack_from("<I", data, off)
    off += 4 + count * 36
off += 4                                                     # colour
(out / f"{mesh_id}.archive").write_bytes(data[off:])
print(f"wrote {out}/{mesh_id}.archive ({len(data) - off} bytes)")
