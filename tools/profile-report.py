#!/usr/bin/env python3
"""Where a Wine process's thread spends its time: perf samples (`perf script -F tid,ip`) bucketed by PE module (from
/proc/PID/maps and each image's SizeOfImage) and, for randy31_orig.dll and our randy31.dll, by function.

Function names: randy31_orig from a Ghidra function list (build/profile/orig-functions.txt, made with
`tools/ghidra-run.sh randy31.dll ListFunctions.java <file>` if missing), ours from build/linux-release/randy31.map.

Usage: tools/profile-report.py samples.txt maps.txt [--thread TID | --rank N] [--top N]
"""
import bisect
import collections
import pathlib
import re
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def modules(maps_path):
    """[(start, end, name)] for every PE image mapped from a file."""
    first = {}
    for line in open(maps_path):
        parts = line.split(None, 5)
        if len(parts) < 6:
            continue
        path = parts[5].strip()
        if not re.search(r"\.(dll|exe|drv)$", path, re.I):
            continue
        start = int(parts[0].split("-")[0], 16)
        if path not in first or start < first[path]:
            first[path] = start
    out = []
    for path, start in first.items():
        try:
            data = open(path, "rb").read(4096)
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            size = struct.unpack_from("<I", data, pe + 24 + 56)[0]       # OptionalHeader.SizeOfImage
        except (OSError, struct.error):
            continue
        out.append((start, start + size, pathlib.Path(path).name))
    return sorted(out)


def orig_functions():
    path = ROOT / "build/profile/orig-functions.txt"
    if not path.exists():
        subprocess.run([str(ROOT / "tools/ghidra-run.sh"), "randy31.dll", "ListFunctions.java", str(path)],
                       cwd=ROOT, capture_output=True)
    funcs = []
    for line in open(path):
        rva, size, name = line.split(" ", 2)
        funcs.append((int(rva, 16), name.strip()))
    return sorted(funcs)


def own_functions():
    funcs = []
    for line in open(ROOT / "build/linux-release/randy31.map", errors="replace"):
        m = re.match(r"\s*0001:([0-9a-f]+)\s+(\S+)\s+([0-9a-f]{8,16})", line)
        if m:
            funcs.append((int(m.group(3), 16) - 0x10000000, m.group(2)))
    return sorted(funcs)


def lookup(funcs, rva):
    keys = [f[0] for f in funcs]
    i = bisect.bisect_right(keys, rva) - 1
    return funcs[i][1] if i >= 0 else "?"


def main():
    args = sys.argv[1:]
    thread = None
    top = 25
    if "--thread" in args:
        thread = args[args.index("--thread") + 1]
    if "--top" in args:
        top = int(args[args.index("--top") + 1])
    samples = []
    for line in open(args[0]):
        parts = line.split()
        if len(parts) >= 2:
            samples.append((parts[0], int(parts[1], 16)))
    per_thread = collections.Counter(t for t, _ in samples)
    if "--rank" in args:                                 # the n-th busiest thread (1 = busiest)
        rank = int(args[args.index("--rank") + 1])
        ranked = per_thread.most_common()
        if rank > len(ranked):
            print(f"no thread #{rank}")
            return
        thread = ranked[rank - 1][0]
    if thread is None:                                   # the busiest thread
        thread = per_thread.most_common(1)[0][0]
    ips = [ip for t, ip in samples if t == thread]
    mods = modules(args[1])
    starts = [m[0] for m in mods]
    per_module = collections.Counter()
    per_func = collections.Counter()
    tables = {"randy31_orig.dll": orig_functions(), "randy31.dll": own_functions()}
    for ip in ips:
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0 and ip < mods[i][1]:
            name = mods[i][2]
            per_module[name] += 1
            if name.lower() in tables:
                per_func[f"{name}!{lookup(tables[name.lower()], ip - mods[i][0])}"] += 1
        else:
            per_module["(other: unix / wine / kernel)"] += 1
    total = max(len(ips), 1)
    print(f"thread {thread}: {len(ips)} samples")
    for name, n in per_module.most_common(12):
        print(f"  {100.0 * n / total:5.1f}%  {name}")
    # randy31.dll by part: the native Randy code (rnative::<part>), the D3D7 backend (rvkproxy), rvk itself, the rest
    # (CRT, STL, unnamed).
    parts = collections.Counter()
    for name, n in per_func.items():
        if not name.lower().startswith("randy31.dll!"):
            continue
        m = re.search(r"@([A-Za-z_0-9]+)@rnative@@", name)
        part = ("rnative::" + m.group(1)) if m else ("rvkproxy (D3D7 backend)" if "@rvkproxy@@" in name
                                                    else "rvk (renderer)" if "@rvk@@" in name else "CRT / STL / other")
        parts[part] += n
    if parts:
        print("randy31.dll by part:")
        for name, n in parts.most_common(14):
            print(f"  {100.0 * n / total:5.1f}%  {name}")
    print("functions:")
    for name, n in per_func.most_common(top):
        print(f"  {100.0 * n / total:5.1f}%  {name}")


main()
