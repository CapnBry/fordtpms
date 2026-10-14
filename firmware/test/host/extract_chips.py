#!/usr/bin/env python3
"""FM-demodulate rtl_433 .cu8 captures (250 kS/s) and print one chip string per
burst ('1' = higher FSK tone). Used to build host test vectors for the decoder."""
import sys
import numpy as np

FS = 250e3
CHIP = FS * 52e-6  # samples per chip

for fn in sys.argv[1:]:
    raw = np.fromfile(fn, dtype=np.uint8).astype(np.float32) - 127.5
    iq = raw[0::2] + 1j * raw[1::2]
    on = np.abs(iq) > 15
    freq = np.angle(iq[1:] * np.conj(iq[:-1]))
    idx = np.flatnonzero(on)
    bursts, start, prev = [], idx[0], idx[0]
    for i in idx[1:]:
        if i - prev > 200:
            bursts.append((start, prev))
            start = i
        prev = i
    bursts.append((start, prev))
    for s, e in bursts:
        if (e - s) < 120 * CHIP:
            continue
        f = np.convolve(freq[s:e], np.ones(5) / 5, 'same')
        mid = np.median(f)
        bits = (f > mid).astype(int)
        edges = np.flatnonzero(np.diff(bits))
        chips = []
        for a, b in zip(edges[:-1], edges[1:]):
            chips += [bits[a + 1]] * max(1, int(round((b - a) / CHIP)))
        print(''.join(map(str, chips)))
