#!/usr/bin/env python3
"""How much of randy31_orig.dll has a native replacement: its functions (a Ghidra function list) grouped by class -
named functions by their class, unnamed ones by the named function before them (MSVC keeps a source file's functions
together) - against docs/port-ledger.tsv. Library code (CRT, STL, libpng, zlib, statically linked D3DX7) is counted apart.

Usage: tools/port-status.py [--classes N]
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FUNCS = ROOT / "build/profile/orig-functions.txt"
GRAPH = ROOT / "build/profile/orig-callgraph.txt"


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
    """{rva: (is a root - exported or referenced other than by calls, set of caller rvas)} (CallGraph.java)."""
    if not GRAPH.exists():
        subprocess.run([str(ROOT / "tools/ghidra-run.sh"), "randy31.dll", "CallGraph.java", str(GRAPH)], cwd=ROOT,
                       capture_output=True)
    graph = {}
    for line in open(GRAPH):
        parts = line.split()
        callers = {int(c, 16) for c in parts[2].split(",")} if len(parts) > 2 else set()
        graph[int(parts[0], 16)] = (parts[1] != "-", callers)
    return graph


def unreachable(graph, done):
    """Functions nothing live reaches any more: not a root, and every caller native (or itself unreachable)."""
    gone = set()
    changed = True
    while changed:
        changed = False
        for rva, (root, callers) in graph.items():
            if rva in done or rva in gone or root:
                continue
            if all(c in done or c in gone for c in callers):
                gone.add(rva)
                changed = True
    return gone


def main():
    show = int(sys.argv[sys.argv.index("--classes") + 1]) if "--classes" in sys.argv else 25
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
        # 0x58200-0x787AE: statically linked D3DX7 (texture loading); past it the CRT glue.
        if LIB.match(name) or (0x58200 <= rva < 0x787AE and name.startswith("FUN_")) or rva >= 0x787AE:
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
