#!/usr/bin/env python3
"""Extracts HMOccluder_t::PreProcessPlayfield's hand-made heightmap corrections from randy31_orig.dll.

The original has them compiled in (#include "HMOCCDATA_<playfield>.i": lines of SETOCC( x, height, z ), each setting
one cell: settled, and its height to height / the heightmap's scale) - straight-line code per playfield. This runs
that code symbolically (the x87 stack of constants and the scale, the cell address as k * width + c) and writes the
table for proxy/native/occluder.cpp. The data is the game's: the output goes to a git-ignored file
(proxy/native/private/occlusion_data.inc), made from your own copy of the DLL.

Usage: tools/gen_occlusion_data.py [randy31_orig.dll] [out]
"""
import pathlib
import re
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DLL = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                   pathlib.Path.home() / ".wine-prk/drive_c/linux/testclient/randy31_orig.dll")
OUT = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "proxy/native/private/occlusion_data.inc")
BASE = 0x10000000
START, END = 0x1003012C, 0x1003E32D               # PreProcessPlayfield's cases; END: the tail they all reach
FTOL = 0x10078A76
CASES = {0x221: 0x1003C985, 0x236: 0x1003C5BE, 0x280: 0x1003AD86, 0x2BC: 0x1003AAB0, 0x2C1: 0x1003296C,
         0x320: 0x10030186}

data = DLL.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
sections = [struct.unpack_from("<8sIIII", data, pe + 24 + struct.unpack_from("<H", data, pe + 20)[0] + i * 40)
            for i in range(struct.unpack_from("<H", data, pe + 6)[0])]


def read(va, fmt):
    rva = va - BASE
    for _, vsize, vaddr, rsize, raw in sections:
        if vaddr <= rva < vaddr + rsize:
            return struct.unpack_from(fmt, data, raw + rva - vaddr)[0]
    raise ValueError(hex(va))


listing = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", f"--start-address={START:#x}",
                          f"--stop-address={END:#x}", str(DLL)], capture_output=True, text=True, check=True).stdout
code = {}
order = []
for line in listing.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
    if not m:
        continue
    address = int(m.group(1), 16)
    operands = re.sub(r"\s*(#.*|<.*>)$", "", m.group(3)).strip()
    code[address] = (m.group(2), operands)
    order.append(address)


class Lin:
    """k * width + c, maybe plus the heights / flags base."""

    def __init__(self, k=0, c=0, base=None):
        self.k, self.c, self.base = k, c, base

    def __add__(self, o):
        assert not (self.base and o.base)
        return Lin(self.k + o.k, self.c + o.c, self.base or o.base)

    def scaled(self, s):
        assert not self.base or s == 1               # (an index register can hold the base, unscaled)
        return Lin(self.k * s, self.c * s, self.base)


def operand(text, regs):
    """A memory operand's address."""
    m = re.match(r"(-?0x[0-9a-f]+|-?\d+)?\((%\w+)?(?:,(%\w+)(?:,(\d))?)?\)$", text)
    assert m, text
    total = Lin(0, int(m.group(1), 0) if m.group(1) else 0)
    if m.group(2):
        total = total + regs[m.group(2)]
    if m.group(3):
        total = total + regs[m.group(3)].scaled(int(m.group(4) or 1))
    return total


def run(entry):
    regs = {r: Lin() for r in ("%eax", "%ebx", "%ecx", "%edx", "%edi")}
    regs["%esi"] = Lin(base="this")
    stack = []                                      # x87: ("c", value) / ("s",) / ("div", value)
    flags, heights = [], []
    i = order.index(entry)
    while i < len(order):                           # (the last case runs on into the tail)
        address = order[i]
        if address >= END:
            break
        op, args = code[address]
        a = [x.strip() for x in re.split(r",(?![^()]*\))", args)] if args else []
        if op == "jmp":
            assert int(a[0], 16) == END, hex(address)
            break
        if op == "movl" and a[0].startswith("0x") and a[0].endswith("(%esi)"):
            field = int(a[0][:-6], 16)
            regs[a[1]] = {8: Lin(1, 0), 4: Lin(base="flags")}[field]
        elif op == "movl" and a[0] == "(%esi)":
            regs[a[1]] = Lin(base="heights")
        elif op == "movl" and a[0].startswith("%") and a[1].startswith("%"):
            regs[a[1]] = regs[a[0]]
        elif op == "imull":
            assert a[0].startswith("$") and len(a) == 3, args
            regs[a[2]] = regs[a[1]].scaled(int(a[0][1:], 0))
        elif op == "shll":
            regs[a[1]] = regs[a[1]].scaled(1 << int(a[0][1:], 0))
        elif op in ("addl", "subl") and a[0].startswith("$") and a[1] in regs and a[1] != "%esi":
            n = int(a[0][1:], 0)
            regs[a[1]] = regs[a[1]] + Lin(0, n if op == "addl" else -n)
        elif op == "incl":
            regs[a[0]] = regs[a[0]] + Lin(0, 1)
        elif op == "movb" and a[1] == "%bl":           # bl = 0xFF, the settled flag
            assert a[0] == "$-0x1", args
        elif op == "movb":
            assert a[0] in ("%bl", "$-0x1"), args
            target = operand(a[1], regs)
            assert target.base == "flags", args
            flags.append((target.k, target.c))
        elif op == "movw":
            assert a[0] == "%ax", args
            target = operand(a[1], regs)
            assert target.base == "heights" and target.k % 2 == 0 and target.c % 2 == 0, args
            assert last_ftol is not None
            heights.append((target.k // 2, target.c // 2, last_ftol))
        elif op == "flds" and a[0] == "0x10(%esi)":
            stack.append(("s",))
        elif op == "flds" and a[0].startswith("0x100"):
            stack.append(("c", read(int(a[0], 16), "<f")))
        elif op == "fldl":
            stack.append(("c", read(int(a[0], 16), "<d")))
        elif op == "fld":
            n = int(re.match(r"%st\((\d)\)", a[0]).group(1))
            stack.append(stack[-1 - n])
        elif op == "fxch":
            n = int(re.match(r"%st\((\d)\)", a[0]).group(1))
            stack[-1], stack[-1 - n] = stack[-1 - n], stack[-1]
        elif op in ("fdivp", "fdivrp"):
            n = int(re.match(r"%st\((\d)\)", a[1]).group(1))
            top, other = stack[-1], stack[-1 - n]
            # AT&T as llvm-objdump prints it: fdivp %st, %st(i): st(i) = st(0) / st(i); fdivrp: st(i) / st(0).
            num, den = (top, other) if op == "fdivp" else (other, top)
            assert num[0] == "c" and den[0] == "s", (hex(address), num, den)
            stack[-1 - n] = ("div", num[1])
            stack.pop()
        elif op == "fdivrl":
            assert stack[-1][0] == "s"
            stack[-1] = ("div", read(int(a[0], 16), "<d"))
        elif op == "fdivs" and a[0] == "0x10(%esi)":
            assert stack[-1][0] == "c"
            stack[-1] = ("div", stack[-1][1])
        elif op == "calll":
            assert int(a[0], 16) == FTOL, args
            value = stack.pop()
            assert value[0] == "div", value
            last_ftol = value[1]
        elif op in ("pushl", "popl", "leal", "addl", "subl", "testl", "cmpl", "xorl", "je", "jne", "jg"):
            raise AssertionError(f"unexpected {op} {args} at {address:#x}")
        else:
            raise AssertionError(f"unhandled {op} {args} at {address:#x}")
        i += 1
    return flags, heights


last_ftol = None
tables = {}
for playfield, entry in sorted(CASES.items()):
    flags, heights = run(entry)
    assert [(k, c) for k, c, _ in heights] == flags, playfield   # SETOCC: the cell's flag, then its height
    tables[playfield] = heights
    print(f"playfield {playfield}: {len(heights)} cells")

OUT.parent.mkdir(parents=True, exist_ok=True)
lines = ["// Generated by tools/gen_occlusion_data.py from randy31_orig.dll - the game's data, not for publishing.",
         "// SETOCC(x, height, z) per playfield, in the original's order."]
for playfield, cells in tables.items():
    lines.append(f"OCC_PLAYFIELD({playfield:#x})")
    for z, x, h in cells:
        lines.append(f"SETOCC({x}, {h!r}, {z})")
    lines.append("OCC_END()")
OUT.write_text("\n".join(lines) + "\n")
print(f"wrote {OUT} ({sum(len(c) for c in tables.values())} cells)")
