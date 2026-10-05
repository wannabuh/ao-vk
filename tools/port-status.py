#!/usr/bin/env python3
"""How much of randy31_orig.dll has a native replacement: its functions (a Ghidra function list) grouped by class -
named functions by their class, unnamed ones by the named function before them (MSVC keeps a source file's functions
together) - against docs/port-ledger.tsv. Library code (CRT, STL, libpng, zlib, statically linked D3DX7) is counted apart.

An exported function counts as live only if something uses it: a module of the client imports it (AO_CLIENT_DIR,
default the installed client; build/profile/client-imports.txt caches the list), our native code calls it (an orig::
binding or its name), serialize.dll finds it by name (every ?Instantiate@<class>@@SAPAV1@PAVObjectArchive_c@fun@@@Z:
GetProcAddress for the class an archive names), or something other than the export table references it.

A call whose call site never runs doesn't keep its callee live either: the call graph cannot see a block a constant
guard keeps from executing, so those edges (DEAD_CALLS) are dropped. The caller stays live - it does run to its guard.

The statically linked VS2010 std::basic_string and std::map / std::set (STL_STRING / STL_MAP) and the game's
serialize library's fun::FindObjectFor<T> instantiations (FUN_FIND) count as library: their named members already
match the library pattern, and these unnamed ones are the same code Ghidra's matcher missed, not Randy's own to port.

Usage: tools/port-status.py [--classes N] [--list CLASS] [--write-gone FILE]   (--list: the functions of CLASS left to
port; --write-gone: the unreachable functions' rvas, one per line, for the proxy's RANDYVK_GONE_TRAP check)
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

# VS2010's std::basic_string, statically linked (its named members - _Tidy, assign, _Chassign, _Inside, the copy ctor -
# already match LIB above, so these are library too, not Randy's own code to port). Ghidra's library matcher missed the
# unnamed ones; the project keeps the string layout itself (proxy/native/vc10.h) instead of porting the STL.
# erase, _Grow, _Copy (its second half), _Resize, assign(string,pos,n), assign(const char*), at, append, resize, grow
# for a format buffer, the (string,pos,n) constructor, operator=, assign/append, find(const char*,pos,n) and its
# strlen wrapper.
STL_STRING = {0x11E08, 0x11EC6, 0x11F52, 0x11FE3, 0x1211B, 0x1468C, 0x178AD, 0x17908, 0x17969, 0x1798F, 0x17A41,
              0x17A6F, 0x17B42, 0x19794, 0x19826}

# VS2010's std::map / std::set red-black tree, also statically linked and unnamed (its node's colour is at +0x2C / +0x2D
# and its header at +4): _Lbound, the two rotations, the header, _Erase, the recursive and range erases and the map
# destructor. Same treatment as the string ones.
STL_MAP = {0x47E91, 0x47F15, 0x47FB1, 0x47FF6, 0x48244, 0x4849B, 0x485B7, 0x485DE, 0x48631}

# The game's serialize library's fun::FindObjectFor<T> instantiations: ArchiveStream_c::DoFindObject then an
# __RTDynamicCast to the wanted class. Statically linked and unnamed, like the STL (every DoFindObject caller here
# carries the Serializable_c RTTI check). 0x4D6D3 (RVisual_t's archive constructor, 0x100 bytes) also finds objects
# but is Randy's own, so it is not here.
FUN_FIND = {0x13D19, 0x13D62, 0x2BDDC, 0x2F989, 0x414DF, 0x460B8, 0x46101, 0x46DB0, 0x47ECC, 0x48BBE, 0x49E12, 0x4AF4F,
            0x4F8AB}


def call_graph():
    """{rva: (is a root, caller rvas, vtable rvas holding it)} and {vtable rva: (function rvas referencing it, other data
    references it)} (CallGraph.java). A root is referenced other than by calls, the export table and vtables, or exported
    and used (client_imports, native_uses, serialize.dll's ?Instantiate@ lookups)."""
    table, (directory, size) = exports()
    if not GRAPH.exists():
        subprocess.run([str(ROOT / "tools/ghidra-run.sh"), "randy31.dll", "CallGraph.java", str(GRAPH),
                        f"{directory:x}", f"{size:x}"], cwd=ROOT, capture_output=True)
    used = client_imports() | native_uses()

    def rvas(field):
        value = field.split("=", 1)[1]
        return {int(v, 16) for v in value.split(",")} if value else set()
    graph, vtables = {}, {}
    for line in open(GRAPH):
        parts = line.split()
        if parts[0] == "vtable":
            vtables[int(parts[1], 16)] = (rvas(parts[2]), parts[3] == "data=1")
            continue
        rva = int(parts[0], 16)
        named = table.get(rva, [])
        root = "d" in parts[1] or ("e" in parts[1] and (not named or any(n in used or n.startswith("?Instantiate@")
                                                                          for n in named)))   # (not named: DllMain)
        graph[rva] = (root, rvas(parts[2]), rvas(parts[3]), rvas(parts[4]))
    return graph, vtables


def called_natively():
    """Original functions our native code still calls by address (Internal<...>(0x...)): live roots."""
    rvas = set()
    for f in (ROOT / "proxy/native").glob("*.cpp"):
        for m in re.finditer(r"Internal<[^;]*?>\(\s*0x([0-9A-Fa-f]+)\s*\)", f.read_text()):
            rvas.add(int(m.group(1), 16))
    return rvas


# Calls whose call site never runs, so they don't keep their callee live even though the caller is: the call graph
# cannot see a block a constant guard keeps from executing. The caller stays live (it runs to its guard); only these
# edges are dropped. FUN_10011ba0 enters the block 0x10011bb9-0x10011c50 only when its second argument is 1 and global
# 0x100b6088 != 1. That global is 1 at load and only ever incremented inside the block, so the block - and every call
# in it - is dead. dd_GetErrorString's only live-looking call (0x10011c45) is in it; so is FUN_10025e17's only call.
DEAD_CALLS = {
    (0x11BA0, 0x14275),   # DynamicVB_c::Get
    (0x11BA0, 0x14177),   # DynamicVB_c::GetVertices
    (0x11BA0, 0x47271),   # RandyShadowlandsData_s::IsGroundLightUsed
    (0x11BA0, 0x787B4),   # operator new
    (0x11BA0, 0x135F1),   # RSprite construction
    (0x11BA0, 0x508D3),   # CATAnimBlend_t construction
    (0x11BA0, 0x51FB2),   # CATKeyframeAnim_t construction
    (0x11BA0, 0x25E17),   # FUN_10025e17 (mov eax, ecx; ret)
    (0x11BA0, 0x1AA09),   # dd_GetErrorString
}


def unreachable(graph, vtables, done, dead):
    """Functions nothing live reaches any more. Live, from the roots (used exports, references that lead nowhere
    traceable, functions our native code calls by address, vtables of static objects): what a live function calls
    (except a dead call site, DEAD_CALLS), takes the address of or names in its exception tables, the vtables it
    references (it makes or destroys such objects) and every function in a live vtable. Replaced and dead functions
    run no original code: they pass nothing on, except that a replaced one still makes its objects (its vtables stay
    live)."""
    callees, owned, members, uses = {}, {}, {}, {}
    for rva, (_, callers, holders, owners) in graph.items():
        for c in callers:
            if (c, rva) not in DEAD_CALLS:
                callees.setdefault(c, set()).add(rva)
        for o in owners:
            owned.setdefault(o, set()).add(rva)
        for v in holders:
            members.setdefault(v, set()).add(rva)
    for v, (users, _) in vtables.items():
        for u in users:
            uses.setdefault(u, set()).add(v)
    live, live_vtables, stack = set(), set(), []

    def mark(f):
        if f not in live:
            live.add(f)
            stack.append(f)

    def mark_vtable(v):
        if v not in live_vtables:
            live_vtables.add(v)
            for f in members.get(v, ()):
                mark(f)
    for v, (_, data) in vtables.items():
        if data:
            mark_vtable(v)
    natively = called_natively()
    for rva, (root, _, _, _) in graph.items():
        if root or rva in natively:
            mark(rva)
    for rva in done - dead:
        for v in uses.get(rva, ()):
            mark_vtable(v)
    while stack:
        f = stack.pop()
        if f in done:
            continue
        for c in callees.get(f, ()):
            mark(c)
        for o in owned.get(f, ()):
            mark(o)
        for v in uses.get(f, ()):
            mark_vtable(v)
    return {rva for rva in graph if rva not in live and rva not in done}


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
    dead = {rva for rva, status in ledger.items() if status == "dead"}
    gone = unreachable(*call_graph(), done, dead)
    if "--write-gone" in sys.argv:
        out = pathlib.Path(sys.argv[sys.argv.index("--write-gone") + 1])
        out.write_text("".join(f"{rva:x}\n" for rva in sorted(gone | dead)))
    groups = {}
    current = "?"
    for rva, size, name in functions():
        # Below 0x11380: libpng (its static functions unnamed); 0x5C1D0-0x6E06A: zlib 1.2.5 and statically linked D3DX7
        # (texture loading); past 0x787AE the CRT glue. 0x6E06A-0x787AE is Funcom's own code linked in - the FAF scene
        # reader (FC_*), streams (SL_*), config lines (LineItem*), matrix helpers - to port like Randy's (it calls
        # Randy's code; the libraries above never do).
        if (LIB.match(name) or rva in STL_STRING or rva in STL_MAP or rva in FUN_FIND or rva < 0x11380
                or (0x5C1D0 <= rva < 0x6E06A and name.startswith("FUN_")) or rva >= 0x787AE):
            cls = "(library)"
        elif 0x6E06A <= rva < 0x787AE:
            cls = "(Funcom FC/SL libraries)"
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
