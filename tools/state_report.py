#!/usr/bin/env python3
"""Summarise which Direct3D 7 render states / texture stage states the client sets, and to what.

Reads interface/state_args_*.tsv (ghidra-scripts/ConstArgs.java output) and writes
interface/state_usage.md. Arguments include the thiscall `this`, so a render state call is
(this, state, value[, priority]) and a texture stage state call is (this, stage, type, value[, priority]).
"""
import collections
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent

RS = {1: "ANTIALIAS", 2: "TEXTUREPERSPECTIVE", 7: "ZENABLE", 8: "FILLMODE", 9: "SHADEMODE",
      10: "LINEPATTERN", 14: "ZWRITEENABLE", 15: "ALPHATESTENABLE", 16: "LASTPIXEL", 19: "SRCBLEND",
      20: "DESTBLEND", 22: "CULLMODE", 23: "ZFUNC", 24: "ALPHAREF", 25: "ALPHAFUNC", 26: "DITHERENABLE",
      27: "ALPHABLENDENABLE", 28: "FOGENABLE", 29: "SPECULARENABLE", 30: "ZVISIBLE", 33: "STIPPLEDALPHA",
      34: "FOGCOLOR", 35: "FOGTABLEMODE", 36: "FOGSTART", 37: "FOGEND", 38: "FOGDENSITY",
      40: "EDGEANTIALIAS", 41: "COLORKEYENABLE", 46: "ZBIAS", 47: "RANGEFOGENABLE", 52: "STENCILENABLE",
      53: "STENCILFAIL", 54: "STENCILZFAIL", 55: "STENCILPASS", 56: "STENCILFUNC", 57: "STENCILREF",
      58: "STENCILMASK", 59: "STENCILWRITEMASK", 60: "TEXTUREFACTOR", 136: "CLIPPING", 137: "LIGHTING",
      138: "EXTENTS", 139: "AMBIENT", 140: "FOGVERTEXMODE", 141: "COLORVERTEX", 142: "LOCALVIEWER",
      143: "NORMALIZENORMALS", 144: "COLORKEYBLENDENABLE", 145: "DIFFUSEMATERIALSOURCE",
      146: "SPECULARMATERIALSOURCE", 147: "AMBIENTMATERIALSOURCE", 148: "EMISSIVEMATERIALSOURCE",
      151: "VERTEXBLEND", 152: "CLIPPLANEENABLE"}
RS.update({128 + i: f"WRAP{i}" for i in range(8)})

TSS = {1: "COLOROP", 2: "COLORARG1", 3: "COLORARG2", 4: "ALPHAOP", 5: "ALPHAARG1", 6: "ALPHAARG2",
       7: "BUMPENVMAT00", 8: "BUMPENVMAT01", 9: "BUMPENVMAT10", 10: "BUMPENVMAT11", 11: "TEXCOORDINDEX",
       12: "ADDRESS", 13: "ADDRESSU", 14: "ADDRESSV", 15: "BORDERCOLOR", 16: "MAGFILTER", 17: "MINFILTER",
       18: "MIPFILTER", 19: "MIPMAPLODBIAS", 20: "MAXMIPLEVEL", 21: "MAXANISOTROPY", 22: "BUMPENVLSCALE",
       23: "BUMPENVLOFFSET", 24: "TEXTURETRANSFORMFLAGS"}

BLEND = dict(enumerate("? ZERO ONE SRCCOLOR INVSRCCOLOR SRCALPHA INVSRCALPHA DESTALPHA INVDESTALPHA DESTCOLOR "
                       "INVDESTCOLOR SRCALPHASAT BOTHSRCALPHA BOTHINVSRCALPHA".split()))
CMP = dict(enumerate("? NEVER LESS EQUAL LESSEQUAL GREATER NOTEQUAL GREATEREQUAL ALWAYS".split()))
TOP = dict(enumerate("? DISABLE SELECTARG1 SELECTARG2 MODULATE MODULATE2X MODULATE4X ADD ADDSIGNED ADDSIGNED2X "
                     "SUBTRACT ADDSMOOTH BLENDDIFFUSEALPHA BLENDTEXTUREALPHA BLENDFACTORALPHA BLENDTEXTUREALPHAPM "
                     "BLENDCURRENTALPHA PREMODULATE MODULATEALPHA_ADDCOLOR MODULATECOLOR_ADDALPHA "
                     "MODULATEINVALPHA_ADDCOLOR MODULATEINVCOLOR_ADDALPHA BUMPENVMAP BUMPENVMAPLUMINANCE "
                     "DOTPRODUCT3".split()))
STENCILOP = dict(enumerate("? KEEP ZERO REPLACE INCRSAT DECRSAT INVERT INCR DECR".split()))
BOOL = {0: "FALSE", 1: "TRUE"}


def arg(v):
    names = {0: "DIFFUSE", 1: "CURRENT", 2: "TEXTURE", 3: "TFACTOR", 4: "SPECULAR"}
    s = names.get(v & 0xF, hex(v & 0xF))
    if v & 0x10: s += "|COMPLEMENT"
    if v & 0x20: s += "|ALPHAREPLICATE"
    return s


def tci(v):
    gen = {0: "", 0x10000: "|CAMERASPACENORMAL", 0x20000: "|CAMERASPACEPOSITION",
           0x30000: "|CAMERASPACEREFLECTIONVECTOR"}
    return f"{v & 0xFFFF}{gen.get(v & 0xFFFF0000, hex(v & 0xFFFF0000))}"


RS_VALUES = {
    "ZENABLE": {0: "FALSE", 1: "TRUE", 2: "USEW"}, "FILLMODE": {1: "POINT", 2: "WIREFRAME", 3: "SOLID"},
    "SHADEMODE": {1: "FLAT", 2: "GOURAUD", 3: "PHONG"}, "SRCBLEND": BLEND, "DESTBLEND": BLEND,
    "CULLMODE": {1: "NONE", 2: "CW", 3: "CCW"}, "ZFUNC": CMP, "ALPHAFUNC": CMP, "STENCILFUNC": CMP,
    "STENCILFAIL": STENCILOP, "STENCILZFAIL": STENCILOP, "STENCILPASS": STENCILOP,
    "FOGTABLEMODE": {0: "NONE", 1: "EXP", 2: "EXP2", 3: "LINEAR"},
    "FOGVERTEXMODE": {0: "NONE", 1: "EXP", 2: "EXP2", 3: "LINEAR"},
    "DIFFUSEMATERIALSOURCE": {0: "MATERIAL", 1: "COLOR1", 2: "COLOR2"},
    "SPECULARMATERIALSOURCE": {0: "MATERIAL", 1: "COLOR1", 2: "COLOR2"},
    "AMBIENTMATERIALSOURCE": {0: "MATERIAL", 1: "COLOR1", 2: "COLOR2"},
    "EMISSIVEMATERIALSOURCE": {0: "MATERIAL", 1: "COLOR1", 2: "COLOR2"},
    "VERTEXBLEND": {0: "DISABLE", 1: "1WEIGHT", 2: "2WEIGHTS", 3: "3WEIGHTS"},
}
TSS_VALUES = {
    "COLOROP": TOP, "ALPHAOP": TOP, "COLORARG1": arg, "COLORARG2": arg, "ALPHAARG1": arg, "ALPHAARG2": arg,
    "TEXCOORDINDEX": tci, "ADDRESS": {1: "WRAP", 2: "MIRROR", 3: "CLAMP", 4: "BORDER"},
    "ADDRESSU": {1: "WRAP", 2: "MIRROR", 3: "CLAMP", 4: "BORDER"},
    "ADDRESSV": {1: "WRAP", 2: "MIRROR", 3: "CLAMP", 4: "BORDER"},
    "MAGFILTER": {1: "POINT", 2: "LINEAR", 3: "FLATCUBIC", 4: "GAUSSIANCUBIC", 5: "ANISOTROPIC"},
    "MINFILTER": {1: "POINT", 2: "LINEAR", 3: "ANISOTROPIC"},
    "MIPFILTER": {1: "NONE", 2: "POINT", 3: "LINEAR"},
    "TEXTURETRANSFORMFLAGS": lambda v: ({0: "DISABLE", 1: "COUNT1", 2: "COUNT2", 3: "COUNT3", 4: "COUNT4"}
                                        .get(v & 0xFF, hex(v)) + ("|PROJECTED" if v & 0x100 else "")),
}


def name_value(table, key, v):
    t = table.get(key)
    if v is None:
        return "?"
    if callable(t):
        return t(v)
    if isinstance(t, dict):
        return t.get(v, hex(v))
    if key.endswith("ENABLE") or key in ("LIGHTING", "CLIPPING", "COLORVERTEX", "LOCALVIEWER",
                                         "NORMALIZENORMALS", "ZWRITEENABLE"):
        return BOOL.get(v, hex(v))
    return hex(v)


def parse(v):
    return None if v in ("?", "") else int(v, 16)


def main():
    rs = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))   # state -> module -> value
    tss = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))  # type -> module -> value
    stages = collections.Counter()
    unknown_state = collections.Counter()
    for path in sorted(ROOT.glob("interface/state_args_*.tsv")):
        for line in path.read_text().splitlines()[1:]:
            module, callee, _, _, args = line.split("\t")
            a = [parse(x) for x in args.split(",")]
            if re.search(r"SetTextureStageState$|FUN_10023adc$", callee):
                if len(a) < 4: continue
                stage, typ, val = a[1], a[2], a[3]
                if typ is None:
                    unknown_state[module + " TSS"] += 1
                    continue
                stages[stage if stage is not None else "?"] += 1
                tss[TSS.get(typ, f"TSS_{typ}")][module.replace(".dll", "")][val] += 1
            else:
                if len(a) < 3: continue
                state, val = a[1], a[2]
                if state is None:
                    unknown_state[module + " RS"] += 1
                    continue
                rs[RS.get(state, f"RS_{state}")][module.replace(".dll", "")][val] += 1

    def table(title, data, values):
        lines = [f"## {title}\n", "| State | Calls by module | Values seen (count) |", "|---|---|---|"]
        for key in sorted(data, key=lambda k: -sum(sum(c.values()) for c in data[k].values())):
            mods = ", ".join(f"{m} {sum(c.values())}" for m, c in sorted(data[key].items()))
            total = collections.Counter()
            for c in data[key].values():
                total.update(c)
            vals = ", ".join(f"{name_value(values, key, v)} ({n})" for v, n in total.most_common())
            lines.append(f"| {key} | {mods} | {vals} |")
        return "\n".join(lines) + "\n"

    out = ["# Direct3D 7 state usage\n",
           "Generated by tools/state_report.py from interface/state_args_*.tsv (constant arguments at every",
           "call of the Randy state setters in all five modules; `?` = value computed at run time).\n",
           f"Non-constant state types (cannot be named statically): {dict(unknown_state)}\n",
           f"Texture stages used: {dict(stages)}\n",
           table("Render states", rs, RS_VALUES), table("Texture stage states", tss, TSS_VALUES)]
    (ROOT / "interface/state_usage.md").write_text("\n".join(out))
    print(f"{len(rs)} render states, {len(tss)} texture stage states -> interface/state_usage.md")


if __name__ == "__main__":
    main()
