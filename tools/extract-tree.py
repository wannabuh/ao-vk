#!/usr/bin/env python3
"""Dump a static mesh (.abiff, rdb_1010001) with its textures as a flat file for rvk_demo --leaf-tree: every mesh
part in the game's coordinates (left-handed, its rest pose), its texture as A8R8G8B8 and how it blends. Uses
ao-assets (AO_ASSETS, default ~/projects/ao-mods/ao-assets) to read the client's rdb.db.

Usage: tools/extract-tree.py <abiff id> <out dir>        e.g. 20842 build/trees   -> <out>/<id>.tree

File (little-endian): "RVKT", u32 version 1, u32 parts; per part: u32 flags (1 blended, 2 alpha tested),
u32 alpha ref, u32 width, u32 height, width*height u32 pixels (0xAARRGGBB), u32 vertices, vertices * 8 floats
(x y z, nx ny nz, u v), u32 indices, indices * u16 (triangle list), padded to 4 bytes.
"""
import io
import json
import os
import pathlib
import struct
import sys

import numpy as np

sys.path.insert(0, os.environ.get("AO_ASSETS", str(pathlib.Path.home() / "projects/ao-mods/ao-assets")))
from aoassets import rdb, space  # noqa: E402
from aoassets.convert import abiff_gltf  # noqa: E402
from PIL import Image  # noqa: E402

COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
DTYPES = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}


def parse_glb(data):
    length = struct.unpack_from("<I", data, 8)[0]
    off, doc, binary = 12, None, b""
    while off < length:
        size, kind = struct.unpack_from("<II", data, off)
        chunk = data[off + 8:off + 8 + size]
        if kind == 0x4E4F534A:
            doc = json.loads(chunk)
        else:
            binary = chunk
        off += 8 + size
    return doc, binary


def accessor(doc, binary, index):
    a = doc["accessors"][index]
    view = doc["bufferViews"][a["bufferView"]]
    n = COMPONENTS[a["type"]]
    dtype = DTYPES[a["componentType"]]
    start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = view.get("byteStride", 0) or n * np.dtype(dtype).itemsize
    raw = np.frombuffer(binary, dtype=np.uint8, count=stride * (a["count"] - 1) + n * np.dtype(dtype).itemsize,
                        offset=start)
    out = np.empty((a["count"], n), dtype=dtype)
    for i in range(a["count"]):
        out[i] = np.frombuffer(raw[i * stride:i * stride + n * np.dtype(dtype).itemsize], dtype=dtype)
    return out


def node_matrix(node):
    if "matrix" in node:
        return np.asarray(node["matrix"], dtype=np.float64).reshape(4, 4)   # column-major = row-vector rows
    m = np.eye(4)
    if "scale" in node:
        m = np.diag(list(node["scale"]) + [1.0]) @ m
    if "rotation" in node:
        x, y, z, w = node["rotation"]                # glTF quaternion; row-vector rotation matrix
        r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
                      [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
                      [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]])
        rm = np.eye(4)
        rm[:3, :3] = r
        m = m @ rm
    if "translation" in node:
        t = np.eye(4)
        t[3, :3] = node["translation"]
        m = m @ t
    return m


def main():
    mesh_id, out = int(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    glb, warnings = abiff_gltf.export(rdb.Rdb(), rdb.T_ABIFF, mesh_id)
    doc, binary = parse_glb(glb)
    parts = []

    def walk(index, parent):
        node = doc["nodes"][index]
        world = node_matrix(node) @ parent
        if "mesh" in node:
            for prim in doc["meshes"][node["mesh"]]["primitives"]:
                parts.append((prim, world))
        for child in node.get("children", []):
            walk(child, world)

    for root in doc["scenes"][doc.get("scene", 0)]["nodes"]:
        walk(root, np.eye(4))

    blob = bytearray(b"RVKT" + struct.pack("<II", 1, len(parts)))
    for prim, world in parts:
        attrs = prim["attributes"]
        pos = accessor(doc, binary, attrs["POSITION"]).astype(np.float64)
        pos = np.c_[pos, np.ones(len(pos))] @ world
        nrm = accessor(doc, binary, attrs["NORMAL"]).astype(np.float64) @ world[:3, :3] if "NORMAL" in attrs \
            else np.tile([0.0, 1.0, 0.0], (len(pos), 1))
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        uv = accessor(doc, binary, attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else np.zeros((len(pos), 2))
        idx = accessor(doc, binary, prim["indices"]).reshape(-1) if "indices" in prim else np.arange(len(pos))
        # Back to the game's space: Z mirrored again, winding reversed again (aoassets.space is its own inverse).
        pos = space.vecs(pos[:, :3])
        nrm = space.vecs(nrm)
        idx = space.tris(idx)
        mat = doc["materials"][prim["material"]] if "material" in prim else {}
        flags, ref = 0, 128
        if mat.get("alphaMode") == "BLEND":
            flags |= 1
        elif mat.get("alphaMode") == "MASK":
            flags |= 2
            ref = int(round(mat.get("alphaCutoff", 0.5) * 255))
        tex = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if tex is not None:
            image = doc["images"][doc["textures"][tex["index"]]["source"]]
            view = doc["bufferViews"][image["bufferView"]]
            png = binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]]
            rgba = np.asarray(Image.open(io.BytesIO(png)).convert("RGBA"), dtype=np.uint32)
        else:
            rgba = np.full((1, 1, 4), 255, dtype=np.uint32)
        h, w = rgba.shape[:2]
        argb = (rgba[..., 3] << 24) | (rgba[..., 0] << 16) | (rgba[..., 1] << 8) | rgba[..., 2]
        blob += struct.pack("<IIII", flags, ref, w, h) + argb.astype("<u4").tobytes()
        verts = np.c_[pos, nrm, uv].astype("<f4")
        blob += struct.pack("<I", len(verts)) + verts.tobytes()
        blob += struct.pack("<I", len(idx)) + idx.astype("<u2").tobytes()
        if len(idx) % 2:
            blob += b"\0\0"
        lo, hi = pos.min(0), pos.max(0)
        print(f"part: {len(verts)} vertices, {len(idx) // 3} triangles, texture {w}x{h}, flags {flags}, "
              f"box ({lo[0]:.2f} {lo[1]:.2f} {lo[2]:.2f}) - ({hi[0]:.2f} {hi[1]:.2f} {hi[2]:.2f})")
    path = out / f"{mesh_id}.tree"
    path.write_bytes(bytes(blob))
    for w in warnings:
        print("warning:", w)
    print(f"wrote {path} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
