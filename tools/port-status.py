#!/usr/bin/env python3
"""How much of randy31_orig.dll has a native replacement: its functions (a Ghidra function list) grouped by class -
named functions by their class, unnamed ones by the named function before them (MSVC keeps a source file's functions
together) - against docs/port-ledger.tsv. Library code (CRT, STL, libpng, zlib, statically linked D3DX7) is counted apart.

An exported function counts as live only if something uses it: a module of the client imports it (AO_CLIENT_DIR,
default the installed client; build/profile/client-imports.txt caches the list), our native code calls it (an orig::
binding or its name), serialize.dll finds it by name (every ?Instantiate@<class>@@SAPAV1@PAVObjectArchive_c@fun@@@Z:
GetProcAddress for the class an archive names), or something other than the export table references it.

Usage: tools/port-status.py [--classes N] [--list CLASS]   (--list: the functions of CLASS left to port)
"""
import os
import pathlib
import re
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FUNCS = ROOT / "build/profile/orig-functions.txt"
GRAPH = ROOT / "build/profile/orig-callgraph.txt"
IMPORTS = ROOT / "build/profile/client-imports.txt"
ORIG = ROOT / "orig/randy31.dll"
CLIENT = pathlib.Path(os.environ.get("AO_CLIENT_DIR", pathlib.Path.home() / ".wine-prk/drive_c/linux/client"))


def pe_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count, optional = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for i in range(count):
        o = pe + 24 + optional + 40 * i
        size, va, _, raw = struct.unpack_from("<IIII", data, o + 8)
        sections.append((va, size, raw))
    return pe, sections


def exports():
    """{rva: [names]} of randy31_orig.dll's export table, and the export directory (rva, size)."""
    data = ORIG.read_bytes()
    pe, sections = pe_sections(data)

    def at(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError(hex(rva))
    directory, size = struct.unpack_from("<II", data, pe + 24 + 96)
    d = at(directory)
    names_count, functions, names, ordinals = struct.unpack_from("<IIII", data, d + 24)
    out = {}
    for i in range(names_count):
        name_rva = struct.unpack_from("<I", data, at(names) + 4 * i)[0]
        end = data.index(b"\0", at(name_rva))
        name = data[at(name_rva):end].decode()
        ordinal = struct.unpack_from("<H", data, at(ordinals) + 2 * i)[0]
        rva = struct.unpack_from("<I", data, at(functions) + 4 * ordinal)[0]
        out.setdefault(rva, []).append(name)
    return out, (directory, size)


def client_imports():
    """Names the client's modules import from randy31.dll (objdump over the client folder)."""
    if not IMPORTS.exists():
        names = set()
        for f in sorted(CLIENT.rglob("*")):
            if f.suffix.lower() not in (".dll", ".exe") or f.name.lower().startswith("randy31"):
                continue
            listing = subprocess.run(["objdump", "-p", str(f)], capture_output=True, text=True).stdout
            inside = False
            for line in listing.splitlines():
                if "DLL Name:" in line:
                    inside = line.split("DLL Name:")[1].strip().lower() == "randy31.dll"
                elif inside and re.match(r"\t[0-9a-f]+ ", line):
                    names.add(line.split()[-1])
        IMPORTS.parent.mkdir(parents=True, exist_ok=True)
        IMPORTS.write_text("".join(n + "\n" for n in sorted(names)))
    return set(IMPORTS.read_text().split())


def native_uses():
    """Exported names our code calls: orig:: bindings (g_fn[i] = kExports[i]) and names looked up by string."""
    gen = (ROOT / "proxy/native/orig_api.gen.cpp").read_text()
    table = re.findall(r'^\s*\{"([^"]+)"\}', gen, re.M)
    header = (ROOT / "proxy/native/orig_api.gen.h").read_text()
    binding = {m.group(1): int(m.group(2)) for m in re.finditer(r"inline [^\n]*? (\w+)\([^\n]*g_fn\[(\d+)\]", header)}
    used = set()
    for f in list((ROOT / "proxy").rglob("*.cpp")) + list((ROOT / "proxy").rglob("*.h")):
        if f.name.startswith("orig_api.gen"):
            continue
        text = f.read_text(errors="replace")
        for name in re.findall(r"orig::(\w+)\(", text):
            if name in binding:
                used.add(table[binding[name]])
        used.update(re.findall(r'"(\?[^"\s]+@@[^"\s]*)"', text))
    return used


def functions():
    if not FUNCS.exists():
        FUNCS.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(ROOT / "tools/ghidra-run.sh"), "randy31.dll", "ListFunctions.java", str(FUNCS)], cwd=ROOT,
                       capture_output=True)
    out = []
    for line in open(FUNCS):
        rva, size, name = line.split(" ", 2)
        out.append((int(rva, 16), int(size, 16), name.strip()))
    return sorted(out)


LIB = re.compile(r"^(png_|inflate|deflate|zlib|adler|crc|_|std::|`|operator|Catch|Unwind|thunk_|MSVCR100|SERIALIZE|DDRAW|"
                 r"FID_conflict|fun::)")


def call_graph():
    """{rva: (is a root, set of caller rvas)} (CallGraph.java): a root is referenced other than by calls and the export
    table, or exported and used (client_imports, native_uses)."""
    table, (directory, size) = exports()
    if not GRAPH.exists():
        subprocess.run([str(ROOT / "tools/ghidra-run.sh"), "randy31.dll", "CallGraph.java", str(GRAPH),
                        f"{directory:x}", f"{size:x}"], cwd=ROOT, capture_output=True)
    used = client_imports() | native_uses()
    graph = {}
    for line in open(GRAPH):
        parts = line.split()
        rva = int(parts[0], 16)
        callers = {int(c, 16) for c in parts[2].split(",")} if len(parts) > 2 else set()
        root = "d" in parts[1] or ("e" in parts[1] and any(n in used or n.startswith("?Instantiate@")
                                                            for n in table.get(rva, [])))
        graph[rva] = (root, callers)
    return graph


def called_natively():
    """Original functions our native code still calls by address (Internal<...>(0x...)): live roots."""
    rvas = set()
    for f in (ROOT / "proxy/native").glob("*.cpp"):
        for m in re.finditer(r"Internal<[^;]*?>\(\s*0x([0-9A-Fa-f]+)\s*\)", f.read_text()):
            rvas.add(int(m.group(1), 16))
    return rvas


def unreachable(graph, done):
    """Functions nothing live reaches any more: not a root, not called by native code, and every caller native (or
    itself unreachable)."""
    live = called_natively()
    gone = set()
    changed = True
    while changed:
        changed = False
        for rva, (root, callers) in graph.items():
            if rva in done or rva in gone or root or rva in live:
                continue
            if all(c in done or c in gone for c in callers):
                gone.add(rva)
                changed = True
    return gone


def main():
    show = int(sys.argv[sys.argv.index("--classes") + 1]) if "--classes" in sys.argv else 25
    listing = sys.argv[sys.argv.index("--list") + 1] if "--list" in sys.argv else None
    ledger = {}
    for line in open(ROOT / "docs/port-ledger.tsv"):
        if line.startswith("#") or not line.strip():
            continue
        rva, status = line.split("\t")[:2]
        ledger[int(rva, 16)] = status
    done = {rva for rva, status in ledger.items() if status in ("replaced", "dead")}
    gone = unreachable(call_graph(), done)
    groups = {}
    current = "?"
    for rva, size, name in functions():
        # Below 0x11380: libpng (its static functions unnamed); 0x58200-0x787AE: statically linked D3DX7 (texture
        # loading); past it the CRT glue.
        if (LIB.match(name) or rva < 0x11380 or (0x58200 <= rva < 0x787AE and name.startswith("FUN_"))
                or rva >= 0x787AE):
            cls = "(library)"
        elif name.startswith("FUN_"):
            cls = current
        else:
            cls = name.split("::")[0] if "::" in name else "(free functions)"
            current = cls
        g = groups.setdefault(cls, [0, 0, 0, 0, 0, 0])   # functions, bytes, replaced (count, bytes), gone (count, bytes)
        g[0] += 1
        g[1] += size
        if rva in done:                                # replaced, or dead (never called): nothing to port
            g[2] += 1
            g[3] += size
        elif rva in gone:                              # only called from replaced code
            g[4] += 1
            g[5] += size
        elif cls == listing:
            print(f"0x{rva:05X} {size:6d}  {name}")
    lib = groups.pop("(library)", [0, 0, 0, 0, 0, 0])
    total = [sum(g[i] for g in groups.values()) for i in range(6)]
    wrapped = sum(1 for s in ledger.values() if s == "wrapped")
    dead = sum(1 for s in ledger.values() if s == "dead")
    print(f"Randy's own code: {total[0]} functions, {total[1] / 1024:.0f} KB; replaced {total[2]} functions "
          f"({total[3] / 1024:.1f} KB, {100.0 * total[3] / max(total[1], 1):.1f}%), {wrapped} more wrapped"
          + (f" (done includes {dead} never called)" if dead else ""))
    left = total[1] - total[3] - total[5]
    print(f"no longer reachable (only called from replaced code, or never): {total[4]} functions "
          f"({total[5] / 1024:.1f} KB); left to port: {total[0] - total[2] - total[4]} functions "
          f"({left / 1024:.1f} KB, {100.0 * left / max(total[1], 1):.1f}%)")
    print(f"library code (not to port): {lib[0]} functions, {lib[1] / 1024:.0f} KB")
    print(f"{'class':32} {'functions':>9} {'KB':>6} {'replaced':>9} {'gone':>5} {'left KB':>8}")
    for cls, g in sorted(groups.items(), key=lambda kv: -(kv[1][1] - kv[1][3] - kv[1][5]))[:show]:
        print(f"{cls[:32]:32} {g[0]:9d} {g[1] / 1024:6.1f} {g[2]:4d} ({100.0 * g[3] / max(g[1], 1):3.0f}%) {g[4]:5d} "
              f"{(g[1] - g[3] - g[5]) / 1024:8.1f}")


main()
