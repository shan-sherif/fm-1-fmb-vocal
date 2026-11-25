#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
"""Estimate the per-sample cost of a function on the FM-1 (pi32v2) from the compiler's assembly (clang -S).

The assembly marks every basic block with its loop: "Loop Header: Depth=d" / "in Loop: Header=BBx Depth=d".
Each block's instructions are counted once per pass of its innermost loop, and a loop at depth d inside the
per-sample loop (depth 1) runs as many times as the trip count given for its header. Both arms of an if are
counted, so this is an upper bound on instructions; divides, square roots and calls are listed apart, as they
cost many cycles each.

  pi_cost.py <file.s> <function> [header=trips ...]   e.g. LBB3_6=46
Without trips, every inner loop is assumed to run `--default` times (46: the male airway's node count)."""
import re
import sys
from collections import defaultdict

path, func = sys.argv[1], sys.argv[2]
trips = {}
default = 46
top = 1            # --top=N: the function's outermost loops run N times a sample (a stage called once a sample)
for a in sys.argv[3:]:
    if a.startswith("--default="):
        default = int(a.split("=")[1])
    elif a.startswith("--top="):
        top = int(a.split("=")[1])
    else:
        k, v = a.split("=")
        trips[k] = int(v)

lines = open(path).read().splitlines()
start = next(i for i, l in enumerate(lines) if l.startswith(func + ":"))
end = next((i for i in range(start + 1, len(lines)) if re.match(r"^\.Lfunc_end", lines[i])), len(lines))

block = "entry"
pending_parent = None
header_of = {"entry": None}       # block -> innermost loop header
depth_of = {"entry": 0}
parent = {}                       # loop header -> enclosing header
count = defaultdict(int)
special = defaultdict(lambda: defaultdict(int))
for l in lines[start + 1:end]:
    m = re.match(r"^(\.LBB\d+_\d+|; BB#\d+):", l)
    if m:
        block = m.group(1).lstrip(".; ").replace("BB#", "BB_")
        header_of[block] = None
        depth_of[block] = 0
    # loop notes sit on the label line or on comment lines right after it
    if m or l.lstrip().startswith(";"):
        h = re.search(r"Loop Header: Depth=(\d+)", l)
        inl = re.search(r"in Loop: Header=(BB\d+_\d+) Depth=(\d+)", l)
        pm = re.search(r"Parent Loop (BB\d+_\d+) Depth=(\d+)", l)
        if pm:
            pending_parent = pm.group(1)
        if h:
            me = block.lstrip("L")
            header_of[block] = me
            depth_of[block] = int(h.group(1))
            if int(h.group(1)) > 1 and pending_parent:
                parent[me] = pending_parent
        elif inl:
            header_of[block] = inl.group(1)
            depth_of[block] = int(inl.group(2))
        if m:
            pending_parent = pm.group(1) if pm else None
        continue
    s = l.strip()
    if not s or s.startswith(("#", ".")):
        continue
    count[block] += 1
    for kind, pat in (("call", r"\bcall\b"), ("div", r"/"), ("sqrt", r"sqrt")):
        if re.search(pat, s):
            special[block][kind] += 1
            if kind == "call":
                special[block]["call " + s.split()[-1]] += 1

def runs(header):
    """passes of a loop per pass of the per-sample loop (depth 1 runs once)"""
    n, h = 1, header
    while h and h in parent:
        n *= trips.get("L" + h, trips.get(h, default))
        h = parent[h]
    return n * trips.get(h, top)

per_sample = 0
calls = defaultdict(float)
by_loop = defaultdict(lambda: [0, 0])
for b, n in count.items():
    h, d = header_of.get(b), depth_of.get(b, 0)
    if d == 0:
        continue                  # outside the per-sample loop: once per call
    k = runs(h)
    per_sample += n * k
    by_loop[h][0] += n
    by_loop[h][1] = k
    for kind, c in special[b].items():
        calls[kind] += c * k
print(f"{func}: ~{per_sample:.0f} instructions a sample (upper bound, both arms of each if)")
for h, (n, k) in sorted(by_loop.items(), key=lambda x: -x[1][0] * x[1][1]):
    print(f"  loop {h:10s} {n:5d} instructions x {k:3d} = {n * k:6d}")
for kind, c in sorted(calls.items(), key=lambda x: -x[1]):
    print(f"  {kind:28s} {c:7.0f} a sample")
