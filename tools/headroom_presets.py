#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Cuts the level trim (firmware/src/preset_trim.h) of every factory preset whose six-note chord (or one note) at
velocity 127 is louder than RMS_T (its loudest 23 ms, RMS) or peaks over PEAK_T dBFS as it enters the master limiter, so a
full chord stays under the limiter instead of being crushed by it (sloop_custom, headroom). RMS decides the sustained sound (a
click or a noise burst is the limiter's, with its look-ahead); PEAK_T keeps the worst of those in reach. Presets under both keep
their trim (level_presets.py's).
The peaks come from tests/chord_probe.c (its trims are in the measurement: run this again until nothing moves):

  cc -O2 -DPROBE_PRELIM -Ibuild/gen -Ifirmware/src -Ifirmware/hal tests/chord_probe.c -lm -o build/host/chord_probe
  build/host/chord_probe > build/host/chords.txt
  tools/headroom_presets.py build/host/chords.txt

then the floor (a sound whose chord is far louder than its notes, a noise or a stab, would be cut until its phrase is
inaudible: its trim comes back up to LUFS_MIN, at most to where level_presets.py had it; the limiter takes that chord):

  build/host/preset_level build/host/presets.wav build/host/presets.txt build/host/presets.raw
  git show HEAD:firmware/src/preset_trim.h > build/host/trim_before.h      (the trims before the cut)
  cp firmware/src/preset_trim.h build/host/trim_cut.h   (and chords.txt: the probe at those trims)
  tools/headroom_presets.py --floor build/host/presets.raw build/host/presets.txt build/host/trim_before.h build/host/trim_cut.h build/host/chords.txt"""
import re
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "firmware/src/preset_trim.h"
RMS_T = -14.0            # dBFS RMS: a saw-like sound with a crest of 9 dB peaks at the limiter's threshold, -5
PEAK_T = 3.0             # dBFS: the limiter takes 8 dB off a peak this high
LO, HI = -40, 23         # the trim's range, 1/2 dB (preset_trim.h: LEVEL 104 + 23 = 127)


def main(probe):
    peaks = {}
    for line in Path(probe).read_text().splitlines():
        m = re.match(r"(\d+) (\d+) (.*?)\s+(-?\d+\.\d)\s+(-?\d+\.\d)\s+(-?\d+\.\d)\s+(-?\d+\.\d)$", line)
        peaks[(int(m[1]), m[3].strip())] = (max(float(m[4]), float(m[5])), max(float(m[6]), float(m[7])))
    text = OUT.read_text()
    moved, worst = 0, -99.0

    def fix(m):
        nonlocal moved, worst
        e, p, name, v = int(m[1]), m[2], m[3], int(m[4])
        pk = peaks.get((e, name))
        if pk is None:
            return m[0]
        pk, rms = pk
        worst = max(worst, rms)
        cut = min(RMS_T - rms, PEAK_T - pk)
        if cut < -0.25:
            nv = max(LO, v + int(round(cut * 2)))
            moved += nv != v
            return f"/* {e}.{p} {name} */ {nv}"
        return m[0]

    text = re.sub(r"/\* (\d+)\.(\d+) (.*?) \*/ (-?\d+)", fix, text)
    OUT.write_text(text)
    print(f"{moved} trims cut, loudest RMS was {worst:.1f} dBFS")


OVER = 4.0               # dB over RMS_T the floor may take a chord
LUFS_MIN = -26.0         # the phrase's loudness a cut sound is not taken under (level_presets.py aims at -15)


def floor(raw, rep, before, cut, chords):
    import numpy as np
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import level_presets as L
    rows = [l.split(None, 3) for l in Path(rep).read_text().splitlines()]
    d = np.fromfile(raw, np.int32).astype(float) / 32768.0
    old = {(int(e), n): int(v) for e, n, v in re.findall(r"/\* (\d+)\.\d+ (.*?) \*/ (-?\d+)", Path(before).read_text())}
    base = {(int(e), n): int(v) for e, n, v in re.findall(r"/\* (\d+)\.\d+ (.*?) \*/ (-?\d+)", Path(cut).read_text())}
    loud = {}                                   # the chord's RMS at the cut trims
    for line in Path(chords).read_text().splitlines():
        m = re.match(r"(\d+) (\d+) (.*?)\s+(-?\d+\.\d)\s+(-?\d+\.\d)\s+(-?\d+\.\d)\s+(-?\d+\.\d)$", line)
        loud[(int(m[1]), m[3].strip())] = max(float(m[6]), float(m[7]))
    lu = {}
    for i, (e, p, role, name) in enumerate(rows):
        x = L.lufs(d[i * L.SEG:(i + 1) * L.SEG])
        if x is not None:
            lu[(int(e), name)] = x
    raised = 0

    def fix(m):
        nonlocal raised
        e, p, name, v = int(m[1]), m[2], m[3], int(m[4])
        x = lu.get((e, name))
        if x is None or x >= LUFS_MIN or v >= old.get((e, name), v):
            return m[0]
        room = int((RMS_T + OVER - loud.get((e, name), RMS_T)) * 2)       # (half dB, from the cut trim)
        nv = min(old[(e, name)], v + int(round((LUFS_MIN - x) * 2)), base.get((e, name), v) + max(room, 0), HI)
        nv = max(nv, v)
        raised += nv != v
        return f"/* {e}.{p} {name} */ {nv}"

    OUT.write_text(re.sub(r"/\* (\d+)\.(\d+) (.*?) \*/ (-?\d+)", fix, OUT.read_text()))
    print(f"{raised} trims raised to a phrase of {LUFS_MIN} LUFS")


if __name__ == "__main__":
    if sys.argv[1] == "--floor":
        floor(*sys.argv[2:7])
    else:
        main(sys.argv[1])
