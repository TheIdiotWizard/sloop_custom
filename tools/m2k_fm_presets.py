#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Writes firmware/src/m2k_fm_presets.h: STAT, PAR and DYN presets from MONO2K's FM sounds.

usage: tools/m2k_fm_presets.py /path/to/mono2k [out.h]

Source: MONO2K firmware/src/m2k_lib.c (its factory library: every sound on an FM machine), m2k_seq.c (each machine's INIT
sound, its page's defaults) and firmware/gen/m2k_tables.h (T_LPK, T_DIST_H). What it makes of a sound:

 * the eight controls as they are (a ratio, MONO2K stores ENC(i, 24): its 24-step index here; TUNE less 64);
 * the amp envelope on sloop's ADSR: ATK (MONO2K's law, 1 ms + 16.4 ms (e^(0.0505 v) - 1)) to sloop's ATK, DEC and REL
   (MONO2K: time to -60 dB) to sloop's DEC and REL (time to 99 %: 2/3 of it); DEC 127 (none) or HOLD 127 (held while the
   gate is open) is a SUS of 127 with the decay as the release; else SUS 0 (a pluck);
 * the filter, which sloop has as one knob on a track (FX > FILTER: FILT, RES, KEY, DIST). MONO2K's follows the note:
   cutoff = the note's frequency x 2^((step - 16) / 8), HP at BASE, LP at BASE + WDTH (the blank kit: BASE 0, WDTH 127,
   so open), LPQ its resonance, DIST its drive. KEY is 100 % for any sound that has one, so the cutoff is its Hz at
   middle C (261.6 Hz x the ratio): FILT is the knob whose cutoff is that; the LP if the sound has one (BASE + WDTH
   under 56: under 8 kHz at middle C), else the HP if BASE is 22 or more (over 1.7 x the note), else flat; RES the
   resonance nearest MONO2K's LPQ in Q (T_LPK); DIST the step nearest MONO2K's gain over 64 (T_DIST_H).
 * not converted: LFOs (the pitch drop of a kick, a vibrato), the delay, the modulation FX, the EQ (sloop's own pages
   do those).
"""
import math, re, sys

root = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else "firmware/src/m2k_fm_presets.h"
lib = open(root + "/firmware/src/m2k_lib.c").read()
seq = open(root + "/firmware/src/m2k_seq.c").read()
tab = open(root + "/firmware/gen/m2k_tables.h").read()


def arr(n):
    m = re.search(r"%s\[\d+\] = \{(.*?)\};" % n, tab, re.S)
    return [int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]


T_LPK, T_DIST_H = arr("T_LPK"), arr("T_DIST_H")
PAR = dict(P_ATK=8, P_HOLD=9, P_DEC=10, P_REL=11, P_DIST=12, P_BASE=16, P_WDTH=17, P_HPQ=18, P_LPQ=19, P_GATE=45)
SYM = dict(SA=0, SB=1, SC=2, SD=3, SE=4, SF=5, SG=6, SH=7)


def num(tok):
    tok = tok.strip()
    m = re.match(r"R24\((\d+)\)$", tok)
    if m:
        return (int(m.group(1)) * 128 + 64) // 24
    if tok in SYM:
        return ("syn", SYM[tok])
    if tok in PAR:
        return ("par", tok)
    if re.match(r"^-?\d+$", tok):
        return int(tok)
    return None


def split_top(s):
    out, d, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            d += 1
        if ch == ")":
            d -= 1
        if ch == "," and d == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return [x.strip() for x in out]


def parse_kv(body):
    """-> {'syn': {k: v}, 'par': {name: v}} from a flat (param, value) list; LFO( ... ) and unknown pairs skipped"""
    toks = split_top(body)
    syn, par = {}, {}
    i = 0
    while i + 1 < len(toks):
        k = num(toks[i])
        v = num(toks[i + 1]) if not toks[i + 1].startswith(("LFO", "MODTY")) else None
        if toks[i].startswith("LFO"):
            i += 1
            continue
        if isinstance(k, tuple) and isinstance(v, int):
            (syn if k[0] == "syn" else par)[k[1]] = v
        i += 2
    return syn, par


# the machines' page defaults (m2k_seq.c MACH_DEF) and INIT sounds
defs = {}
for name, key in (("FM+STAT", "M_FMS"), ("FM+DYN", "M_FMD"), ("FM+PAR", "M_FMP")):
    m = re.search(r"/\* %s\s*\*/ \{([^}]*)\}" % re.escape(name), seq)
    defs[key] = [int(x) for x in m.group(1).split(",")]
inits = {}
for key in ("M_FMS", "M_FMP", "M_FMD"):
    m = re.search(r"\[%s\] = \{\d+, KV\((.*?)\)\}," % key, seq, re.S)
    inits[key] = parse_kv(" ".join(m.group(1).split()))

# the library: S("NAME", CAT, M_x, root, "what", KV...)
sounds = []
for m in re.finditer(r'S\("(\w+)", (\w+), (M_FM[SPD]), (\d+), "[^"]*",', lib):
    i, d = m.end(), 1
    while d:
        d += {"(": 1, ")": -1}.get(lib[i], 0)
        i += 1
    sounds.append((m.group(1), m.group(3), int(m.group(4)), parse_kv(" ".join(lib[m.end():i - 1].split()))))

A_K, A_B = 0.016379, 0.050519


def n_atk(v):
    return 0.001 if v == 0 else 0.001 + A_K * (math.exp(A_B * v) - 1)


def n_dec(v):
    return 0.004 + 2 * A_K * (math.exp(A_B * v) - 1)


def sloop_v(ms):
    return max(0, min(127, round(127 * math.log10(max(ms, 1.0)) / 4)))


def amp(par):
    atk, hold, dec, rel = par.get("P_ATK", 0), par.get("P_HOLD", 0), par.get("P_DEC", 64), par.get("P_REL", 64)
    # (the blank kit's ATK HOLD DEC REL: 0 0 64 64, on format 16's laws: DEC and REL 64 -> 79)
    if "P_DEC" not in par:
        dec = 79
    if "P_REL" not in par:
        rel = 79
    a = sloop_v(n_atk(atk) * 1000)
    held = dec >= 127 or hold >= 127
    if held:
        r = sloop_v(n_dec(min(dec, 126)) * 1000 * 2 / 3) if dec < 127 else sloop_v(n_dec(rel) * 1000 * 2 / 3)
        return [a, 64, 127, r]
    return [a, sloop_v(n_dec(dec) * 1000 * 2 / 3), 0, sloop_v(n_dec(rel) * 1000 * 2 / 3)]


# the sloop filter knob (gen_tables.py: 30 Hz .. 16 kHz over index 0..127; fx.c djf_block: the knob moves it 1.40625)
def knob_lp(hz):
    idx = 127 * math.log(hz / 30) / math.log(16000 / 30)
    return max(-64, min(-1, round((idx - 127) / 1.40625)))


def knob_hp(hz):
    idx = 127 * math.log(hz / 30) / math.log(16000 / 30)
    return max(1, min(63, round(idx / 1.40625)))


RES_Q = [0.72] + [0.72 + (2.55 - 0.72) * r / 10 for r in range(1, 11)] + [4, 8, 16, 32, 64]
TD_G = [1, 2, 4, 10, 32]        # fx.c TDIST_G at 0, 25, 50, 75, 100 %


def my_gain(st):
    q = st * 4
    seg = min(q // 10, 3)
    return TD_G[seg] + (TD_G[seg + 1] - TD_G[seg]) * (q - seg * 10) / 10


def filt(par):
    base, wdth = par.get("P_BASE", 0), par.get("P_WDTH", 127)
    lq, dist = par.get("P_LPQ", 0), par.get("P_DIST", 0)
    tf = tr = tk = td = 0
    notes = []
    if base + wdth < 56:                   # (an LP over ~8 kHz at middle C is out of the way: flat)
        hz = 261.63 * 2 ** ((base + wdth - 16) / 8)
        tf = knob_lp(hz)
        q = 2 ** 24 / T_LPK[lq]
        tr = min(range(16), key=lambda r: abs(math.log(RES_Q[r] / q)))
        tk = 10
        notes.append("LP %.0f Hz at C4, Q %.1f" % (hz, q))
    elif base >= 22:
        hz = 261.63 * 2 ** ((base - 16) / 8)
        tf = knob_hp(hz)
        tk = 10
        notes.append("HP %.0f Hz at C4" % hz)
    if dist > 64:
        g = T_DIST_H[dist] / 2 ** 24
        td = min(range(1, 11), key=lambda st: abs(math.log(max(g, 1e-3) / my_gain(st))))
        notes.append("DIST %d: x%.1f" % (dist, g))
    return [tf, tr, tk, td], notes


def make(name, key, cfg, mono, par_over=None):
    syn, par = cfg
    knobs = list(defs[key])
    for k, v in syn.items():
        knobs[k] = v
    e = []
    for k in range(8):
        v = knobs[k]
        ratio = (key == "M_FMS" and k in (0, 4)) or (key == "M_FMP" and k in (0, 2, 4))
        if ratio:
            v = (v * 24 + 8) >> 7                   # fr24: the 24-step index
        elif k == 7:
            v -= 64
        e.append(v)
    return name, e, amp(par), filt(par), mono


ENG = {"M_FMS": ("STAT", "m2k_stat_presets"), "M_FMP": ("PAR", "m2k_par_presets"), "M_FMD": ("DYN", "m2k_dyn_presets")}
ORDER = {"M_FMS": ["DXBASS", "FMBELL", "FMPAD", "WOODBLK", "FMSNARE"],
         "M_FMP": ["GLASSKY", "GLASPAD", "TING", "COWBELL", "FMHAT"],
         "M_FMD": ["EPIANO", "GROWL", "DRONE", "BUBBLES", "ZAP", "CONGA", "FMKICK"]}
INITN = {"M_FMS": "FM KEYS", "M_FMP": "FM METAL", "M_FMD": "FM BASS"}
MONO = {"DXBASS", "GROWL", "FM BASS", "FMKICK", "DRONE"}
lines = ["/* SPDX-License-Identifier: GPL-3.0-only */",
         "/* Generated by tools/m2k_fm_presets.py from MONO2K's FM sounds (m2k_lib.c, m2k_seq.c); do not edit. Each preset:",
         " * controls, ADSR, then the track filter (FILT RES KEY DIST: the cutoff, resonance, key tracking and drive MONO2K's own",
         " * filter had, approximated: see the tool). */"]
report = []
for key, (en, var) in ENG.items():
    lst = []
    for n in ORDER[key]:
        s = next(x for x in sounds if x[0] == n and x[1] == key)
        lst.append(make(n, key, s[3], 1 if n in MONO else 0))
    lst.append(make(INITN[key], key, inits[key], 1 if INITN[key] in MONO else 0))
    lines.append("static const preset_t %s[] = {" % var.upper())
    for name, e, env, (fl, notes), mono in lst:
        lines.append('    {.name = "%s", .e = {%s}, .env = {%s}, .mono = %d, .flt = {%s}},   /* %s */'
                     % (name, ", ".join(map(str, e)), ", ".join(map(str, env)), mono, ", ".join(map(str, fl)),
                        "; ".join(notes) or "no filter"))
        report.append("%s %-8s %s" % (en, name, "; ".join(notes) or "no filter"))
    lines.append("};")
open(out, "w").write("\n".join(lines) + "\n")
print("\n".join(report))
