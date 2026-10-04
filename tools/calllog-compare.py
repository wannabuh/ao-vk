#!/usr/bin/env python3
"""Compares two call logs (RANDYVK_CALLLOG): lines must be equal, except that the numbers on XF / MAT / LIGHT lines
(transforms, materials, lights - floats a port may round differently from the original's x87 code) may differ by a
relative 1e-5. Prints the first differences; exit status 1 if there are any.

Usage: tools/calllog-compare.py <a> <b>
"""
import sys


def close(x, y):
    try:
        a, b = float(x), float(y)
    except ValueError:
        return x == y
    return abs(a - b) <= 1e-5 * max(1.0, abs(a), abs(b))


def same(l1, l2):
    if l1 == l2:
        return True
    t1, t2 = l1.split(), l2.split()
    if not t1 or t1[0] not in ("XF", "MAT", "LIGHT") or len(t1) != len(t2):
        return False
    return all(close(a, b) for a, b in zip(t1, t2))


a = open(sys.argv[1]).read().splitlines()
b = open(sys.argv[2]).read().splitlines()
bad = 0
if len(a) != len(b):
    print(f"{len(a)} lines / {len(b)} lines")
    bad += 1
for i, (l1, l2) in enumerate(zip(a, b)):
    if not same(l1, l2):
        bad += 1
        if bad <= 5:
            print(f"line {i + 1}:\n< {l1[:200]}\n> {l2[:200]}")
sys.exit(1 if bad else 0)
