#!/usr/bin/env python3
"""Dump MSVC RTTI class hierarchies from 32-bit PE files.

For every class with a Class Hierarchy Descriptor, prints the class and its base classes (with the
offset of each base inside the object), plus the vtable address(es) found through Complete Object
Locators. With --randy, only classes that have a randy31.dll class as a base are printed.

Usage: tools/rtti.py [--randy RANDY_DLL] [--tsv OUT] PE...
"""
import argparse
import struct
import subprocess
import sys


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.data, e_lfanew + 6)[0]
        optsize = struct.unpack_from("<H", self.data, e_lfanew + 20)[0]
        opt = e_lfanew + 24
        self.base = struct.unpack_from("<I", self.data, opt + 28)[0]
        self.sections = []
        for i in range(nsec):
            off = opt + optsize + 40 * i
            name = self.data[off:off + 8].rstrip(b"\0").decode()
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", self.data, off + 8)
            self.sections.append((name, va, max(vsize, rawsize), rawptr, rawsize))

    def off(self, va):
        rva = va - self.base
        for _, sva, size, raw, rawsize in self.sections:
            if sva <= rva < sva + size and rva - sva < rawsize:
                return raw + rva - sva
        return None

    def u32(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from("<I", self.data, o)[0]

    def cstr(self, va):
        o = self.off(va)
        if o is None:
            return None
        end = self.data.find(b"\0", o)
        return self.data[o:end].decode("latin-1")

    def section_range(self, name):
        for n, va, size, raw, rawsize in self.sections:
            if n == name:
                return raw, raw + rawsize, va


def demangle_td(name):
    # ".?AVRVisual_t@@" -> RVisual_t ; nested "?AVFoo@Bar@@" -> Bar::Foo
    if not name.startswith(".?A"):
        return name
    body = name[4:].rstrip("@")
    parts = [p for p in body.split("@") if p]
    return "::".join(reversed(parts)) if parts and not any(c in body for c in "?$") else name


def hierarchies(pe):
    """Returns {class: (bases[(name, mdisp)], vtables[va])}."""
    data = pe.data
    # Type descriptors: pVFTable, spare, ".?AV..." name. Index by VA.
    tds = {}
    pos = 0
    while True:
        i = data.find(b".?A", pos)
        if i < 0:
            break
        pos = i + 3
        va = None
        for n, sva, size, raw, rawsize in pe.sections:
            if raw <= i < raw + rawsize:
                va = pe.base + sva + i - raw
        if va is None:
            continue
        end = data.find(b"\0", i)
        tds[va - 8] = data[i:end].decode("latin-1")

    def td_name(va):
        return demangle_td(tds[va]) if va in tds else None

    # Complete Object Locators: sig 0, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor.
    classes = {}
    vtables = {}
    for name in (".rdata", ".data"):
        r = pe.section_range(name)
        if not r:
            continue
        start, end, sva = r
        for o in range(start, end - 20, 4):
            sig, offset, cd, ptd, pchd = struct.unpack_from("<IIIII", data, o)
            if sig != 0 or ptd not in tds:
                continue
            nbases_ptr = pe.u32(pchd + 8)
            barr = pe.u32(pchd + 12)
            if nbases_ptr is None or barr is None or not 0 < nbases_ptr < 64:
                continue
            col_va = pe.base + sva + o - start
            bases = []
            for k in range(nbases_ptr):
                bcd = pe.u32(barr + 4 * k)
                if bcd is None:
                    break
                bname = td_name(pe.u32(bcd) or 0)
                mdisp = pe.u32(bcd + 8)
                if bname is None:
                    break
                bases.append((bname, mdisp))
            cls = td_name(ptd)
            if not bases or bases[0][0] != cls:
                continue
            classes[cls] = bases
            # vtable: the dword pointing at this COL is vftable[-1]
            needle = struct.pack("<I", col_va)
            j = data.find(needle)
            while j >= 0:
                for n, s2, size, raw, rawsize in pe.sections:
                    if raw <= j < raw + rawsize and n in (".rdata", ".data"):
                        vtables.setdefault(cls, []).append((pe.base + s2 + j - raw + 4, offset))
                j = data.find(needle, j + 4)
    return classes, vtables


def randy_classes(path):
    out = subprocess.run(["llvm-readobj", "--coff-exports", path], capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("Name: ?"):
            mangled = line[6:]
            # ?Method@Class@@... or ??0Class@@...
            body = mangled[3:] if mangled.startswith("??") else mangled[1:]
            parts = body.split("@@")[0].split("@")
            if mangled.startswith("??") and parts:
                names.add(parts[0])
            elif len(parts) >= 2:
                names.add(parts[1])
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--randy", help="randy31.dll: only show classes deriving from its classes")
    ap.add_argument("--tsv", help="write module, class, base, base_offset rows here")
    ap.add_argument("pe", nargs="+")
    a = ap.parse_args()
    rc = randy_classes(a.randy) if a.randy else None
    tsv = open(a.tsv, "w") if a.tsv else None
    if tsv:
        tsv.write("module\tclass\tbase\tbase_offset\n")
    for path in a.pe:
        pe = PE(path)
        classes, vtables = hierarchies(pe)
        mod = path.rsplit("/", 1)[-1]
        print(f"== {mod}: {len(classes)} classes with RTTI")
        for cls in sorted(classes):
            bases = classes[cls]
            if rc is not None and not any(b in rc for b, _ in bases[1:]):
                continue
            desc = ", ".join(f"{b}@+0x{m:X}" + ("*" if rc and b in rc else "") for b, m in bases[1:])
            vt = ", ".join(f"0x{v:08X}(+0x{o:X})" for v, o in vtables.get(cls, []))
            print(f"  {cls}: {desc}   vtables {vt}")
            if tsv:
                for b, m in bases[1:]:
                    tsv.write(f"{mod}\t{cls}\t{b}\t0x{m:X}\n")


if __name__ == "__main__":
    main()
