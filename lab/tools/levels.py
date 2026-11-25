#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
# Loudness over time (dBFS RMS per 100 ms) of 16-bit mono WAVs side by side: levels.py a.wav b.wav [...]
import math, struct, sys
def rd(p):
    b = open(p, 'rb').read(); d = b[44:]; return [v / 32768 for v in struct.unpack('<%dh' % (len(d) // 2), d)]
xs = [rd(p) for p in sys.argv[1:]]
sr, w = 44100, 4410
print(" t(s) " + "".join("%9s" % p.split('/')[-1][:8] for p in sys.argv[1:]))
for k in range(0, min(map(len, xs)) - w, w):
    print("%5.1f " % (k / sr) + "".join("%9.1f" % (20 * math.log10(math.sqrt(sum(v * v for v in x[k:k + w]) / w) + 1e-9)) for x in xs))
