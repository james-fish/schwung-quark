#!/usr/bin/env python3
"""Quark: the single source of truth.

Parameters, pages, the texture-type recipes, wavetable spectra and the factory bank all live
here. Running this script writes:

  src/dsp/params_gen.h / params_gen.c   C parameter table (key, kind, range, default, options)
  src/dsp/tables_gen.h                  texture recipes, wavetable spectra, FM recipes
  src/dsp/contract_gen.c                chain_params + ui_hierarchy JSON for the Schwung host
  src/dsp/presets_gen.c                 factory bank (full value arrays)
  build/web/meta_gen.js                 the same facts for canvas.js / cards.js / the browser UI
  build/chain_params.json, build/ui_hierarchy.json (pretty, for review)

Rules: enum options and param keys are append-only once presets exist (values are stored by index).
Short names <= 5 chars, short options <= 4 chars (Schwung knob grid budget).
"""
import base64
import json
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
DSP = os.path.join(SRC, "dsp")
BUILD = os.path.join(ROOT, "build")

# ------------------------------------------------------------------------------------------------
# Texture types
# family: 0 = strings (divide-down string machine), 1 = wavetable, 2 = FM
# ------------------------------------------------------------------------------------------------

# A divide-down register point: gains of the seven taps, then tone.
#   s16 s8 s4  = sawtooth at 16', 8', 4'      q16 q8 q4 q2 = square at 16', 8', 4', 2'
#   bright     = one-pole tone cutoff, semitones above the played note
#   form       = amount of the vowel formant bank on the texture bus
#   vowel      = 0 oo, .25 oh, .5 ah, .75 eh, 1 ee
#   swell      = semitones the tone opens with the Mod Env (brass swell)
REG_FIELDS = ["s16", "s8", "s4", "q16", "q8", "q4", "q2", "bright", "form", "vowel", "swell"]


def reg(s16=0, s8=0, s4=0, q16=0, q8=0, q4=0, q2=0, bright=48, form=0, vowel=0.5, swell=0):
    return [s16, s8, s4, q16, q8, q4, q2, bright, form, vowel, swell]


STRING_TYPES = {
    # string-machine register sweep: violin -> viola -> cello -> brass -> organ -> choir
    "Strings": [
        reg(s8=1.0, s4=0.55, bright=56, form=0.22, vowel=0.62),          # violin
        reg(s16=0.25, s8=1.0, s4=0.25, bright=46, form=0.18, vowel=0.5),  # viola
        reg(s16=0.95, s8=0.7, bright=34, form=0.14, vowel=0.3),          # cello
        reg(s16=0.45, s8=1.0, s4=0.1, bright=16, swell=40),              # brass
        reg(q16=0.55, q8=0.8, q4=0.5, q2=0.3, bright=64),                # organ
        reg(s16=0.2, s8=0.7, q8=0.35, bright=42, form=1.0, vowel=0.5),   # choir
    ],
    # vintage ensemble strings: dark 16'+8' to airy 8'+4'+2'
    "Vintage": [
        reg(s16=0.7, s8=0.8, bright=26, form=0.1, vowel=0.3),
        reg(s16=0.3, s8=1.0, s4=0.35, bright=40, form=0.12, vowel=0.5),
        reg(s8=1.0, s4=0.6, q2=0.25, bright=60, form=0.1, vowel=0.7),
    ],
    "Brass": [
        reg(s16=0.5, s8=1.0, bright=8, swell=30),
        reg(s16=0.3, s8=1.0, s4=0.2, bright=18, swell=44),
        reg(s8=1.0, s4=0.45, q4=0.15, bright=30, swell=50),
    ],
    "Organ": [
        reg(q16=0.8, q8=1.0, bright=60),
        reg(q16=0.5, q8=0.85, q4=0.65, q2=0.25, bright=64),
        reg(q16=0.65, q8=0.8, q4=0.7, q2=0.65, bright=70),
        reg(s8=0.35, q8=0.6, q4=0.8, q2=0.8, bright=72),
    ],
    "Choir": [
        reg(s16=0.25, s8=0.6, q8=0.4, bright=38, form=1.0, vowel=0.0),
        reg(s16=0.25, s8=0.6, q8=0.4, bright=40, form=1.0, vowel=0.25),
        reg(s16=0.25, s8=0.6, q8=0.4, bright=42, form=1.0, vowel=0.5),
        reg(s16=0.25, s8=0.6, q8=0.4, bright=44, form=1.0, vowel=0.75),
        reg(s16=0.25, s8=0.6, q8=0.4, bright=46, form=1.0, vowel=1.0),
    ],
}

# Wavetables: 8 keyframes, spectra of NH harmonics (sine phases unless 'cos').
NH = 256
WT_FRAMES = 8


def _norm_frames(frames):
    return [[round(a, 5) for a in f] for f in frames]


def wt_polysaw(t):
    return [1.0 / h + (0.75 * t / (h / 2) if h % 2 == 0 else 0.0) * (1 if h < 120 else 0.5) for h in range(1, NH + 1)]


def wt_square(t):
    w = 0.5 - 0.44 * t
    return [(2.0 / (math.pi * h)) * math.sin(math.pi * h * w) for h in range(1, NH + 1)]


ORGAN_SETS = [
    {1: 1}, {1: 1, 2: .5}, {1: 1, 2: .7, 3: .5}, {1: 1, 3: .8, 4: .5},
    {1: .8, 2: .8, 3: .6, 4: .6, 6: .3}, {1: 1, 2: 1, 3: 1, 4: .8, 6: .6, 8: .5},
    {1: .6, 4: 1, 6: .8, 8: .8}, {1: 1, 2: 1, 3: 1, 4: 1, 5: .7, 6: 1, 8: 1, 10: .5, 12: .4},
]


def wt_drawbar(k):
    s = ORGAN_SETS[k]
    return [s.get(h, 0.0) for h in range(1, NH + 1)]


def wt_reed(t):
    out = []
    for h in range(1, NH + 1):
        odd = h % 2 == 1
        slope = 2.0 - 1.15 * t
        a = 1.0 / h ** slope
        if not odd:
            a *= 0.12 + 0.5 * t
        out.append(a)
    return out


def wt_resonant(t):
    c = 2 ** (1.0 + 5.3 * t)
    out = []
    for h in range(1, NH + 1):
        d = (math.log2(h) - math.log2(c)) / 0.16
        peak = 7.0 * math.exp(-0.5 * d * d)
        lp = 1.0 / (1.0 + (h / (c * 1.4)) ** 4)
        out.append((1.0 / h) * (lp + peak))
    return out


def wt_mallet(t):
    parts = {1: 1.0, 4: 0.25 + 0.55 * t, 10: 0.35 * t, 9: 0.12 * t, 16: 0.25 * t * t, 3: 0.08 * (1 - t), 24: 0.12 * t * t}
    return [parts.get(h, 0.0) for h in range(1, NH + 1)]


def wt_bell(t):
    parts = {1: 1.0, 2: 0.25 + 0.2 * t, 3: 0.5 * t, 5: 0.45, 7: 0.4 * t, 11: 0.32 * t, 13: 0.2 * t * t,
             17: 0.18 * t * t, 19: 0.1 * t}
    return [parts.get(h, 0.0) for h in range(1, NH + 1)]


def wt_addharm(t):
    top = 1.0 + 127.0 * (t ** 2.2)
    return [(1.0 / h ** 0.85) / (1.0 + math.exp((h - top) * 0.8)) for h in range(1, NH + 1)]


VOWELS = [(300, 870, 2240), (570, 840, 2410), (730, 1090, 2440), (530, 1840, 2480), (270, 2290, 3010)]


def wt_vocal(t):
    x = t * (len(VOWELS) - 1)
    i = min(int(x), len(VOWELS) - 2)
    f = x - i
    F = [VOWELS[i][j] * (1 - f) + VOWELS[i + 1][j] * f for j in range(3)]
    f0 = 165.0
    out = []
    for h in range(1, NH + 1):
        fr = h * f0
        env = 0.02
        for j, (Fc, g) in enumerate(zip(F, (1.0, 0.7, 0.35))):
            bw = 80 + 40 * j
            env += g * math.exp(-0.5 * ((fr - Fc) / bw) ** 2)
        out.append(env / h ** 0.5)
    return out


def wt_glass(t):
    parts = {1: 1.0, 2: 0.1, 3: 0.35 * t, 5: 0.3 * t, 8: 0.28 * t, 12: 0.24 * t, 17: 0.2 * t * t, 24: 0.16 * t * t,
             33: 0.1 * t * t}
    return [parts.get(h, 0.0) for h in range(1, NH + 1)]


def frames_from(fn, by_index=False):
    return [fn(k) if by_index else fn(k / (WT_FRAMES - 1)) for k in range(WT_FRAMES)]


WAVETABLES = [
    ("Poly Saw", frames_from(wt_polysaw), 0),
    ("Square", frames_from(wt_square), 1),
    ("Drawbar", frames_from(wt_drawbar, True), 0),
    ("Reed", frames_from(wt_reed), 0),
    ("Resonant", frames_from(wt_resonant), 0),
    ("Mallet", frames_from(wt_mallet), 0),
    ("Bellish", frames_from(wt_bell), 0),
    ("Add Harm", frames_from(wt_addharm), 0),
    ("Vocal", frames_from(wt_vocal), 0),
    ("Glass", frames_from(wt_glass), 0),
]

# Chord stacks (the 90s rompler trick: a whole voicing in one sample). One cycle of the table holds DIV
# periods of the played note, so each chord tone sits on an integer harmonic: tone s semitones up is
# harmonic round(DIV * 2^(s/12)), DIV chosen so every tone lands within ~15 cents of equal temperament
# (a touch of just-intonation shimmer). Each tone is a saw (or odd-harmonic pulse) rolled off by TILT;
# the eight frames stack the voicing up, so Variation adds intervals. The oscillator plays the table at
# note/DIV. (name, short, DIV, tilt, odd-only, frames [(name, semitones)])
CHORDS = [
    ("Fifths", "5ths", 4, 1.0, 0, [("Root", [0]), ("5th", [0, 7]), ("5 + 8", [0, 7, 12]), ("5 + 12", [0, 7, 12, 19]),
                                   ("5 + 9", [0, 7, 14, 19]), ("5ths x4", [0, 7, 14, 19, 24]),
                                   ("5ths x5", [0, 7, 12, 14, 19, 26]), ("Sub 5ths", [-12, 0, 7, 14, 19, 24])]),
    ("Minor 9", "Min9", 22, 1.2, 0, [("5th", [0, 7]), ("m", [0, 3, 7]), ("m7", [0, 3, 7, 10]), ("m9", [0, 3, 7, 10, 14]),
                                     ("m11 open", [0, 7, 10, 14, 17]), ("m11", [0, 3, 7, 10, 14, 17]),
                                     ("m11 wide", [0, 7, 10, 15, 17, 22]), ("Sub m11", [-12, 0, 7, 10, 15, 17, 22, 26])]),
    ("Major 9", "Maj9", 8, 1.0, 0, [("5th", [0, 7]), ("Maj", [0, 4, 7]), ("Maj7", [0, 4, 7, 11]), ("Maj9", [0, 4, 7, 11, 14]),
                                    ("Maj9 open", [0, 7, 11, 14, 16]), ("Maj9 wide", [0, 7, 11, 14, 16, 19]),
                                    ("Maj13", [0, 4, 7, 11, 14, 21]), ("Sub Maj9", [-12, 0, 7, 11, 14, 16, 19, 23])]),
    ("Dom 11", "Dm11", 24, 1.1, 0, [("5th", [0, 7]), ("Maj", [0, 4, 7]), ("7", [0, 4, 7, 10]), ("9", [0, 4, 7, 10, 14]),
                                    ("9sus4", [0, 5, 7, 10, 14]), ("11", [0, 7, 10, 14, 17]), ("13", [0, 4, 7, 10, 14, 17, 21]),
                                    ("Sub 11", [-12, 0, 7, 10, 14, 17, 21, 24])]),
    ("Fourths", "4ths", 18, 1.0, 1, [("4th", [0, 5]), ("4ths x3", [0, 5, 10]), ("4ths x4", [0, 5, 10, 15]),
                                     ("4ths x5", [0, 5, 10, 15, 20]), ("So What", [0, 5, 10, 15, 19]),
                                     ("So What 8", [0, 5, 10, 15, 19, 24]), ("4ths x6", [0, 5, 10, 15, 20, 25]),
                                     ("Sub 4ths", [-12, 0, 5, 10, 15, 19, 24])]),
]
CH_TONES = 8


def chord_tones(div, semis):
    """[(harmonic, weight)] for one frame; lower tones carry the weight, upper ones sit back."""
    out = []
    for s in semis:
        h = round(div * 2 ** (s / 12.0))
        cents = 1200 * math.log2(h / div) - 100 * s
        assert abs(cents) <= 16, (div, s, cents)
        w = 0.85 if s < 0 else 1.0 if s == 0 else 0.75 if s < 12 else 0.6 if s < 24 else 0.5
        out.append((h, w))
    assert len(out) <= CH_TONES and len(set(h for h, _ in out)) == len(out)
    return out


assert all(len(c[5]) == WT_FRAMES for c in CHORDS)

# FM: carrier 1:1; mod1 ratio r1 (index = Variation * imax, + Depth env), mod2 ratio r2 with its own
# decaying index i2 over d2 seconds (0 = sustained), feedback on mod1.
FM_FIELDS = ["r1", "imax", "r2", "i2", "d2", "fb"]
FM_TYPES = {
    "E.Piano": [1.0, 2.2, 14.0, 1.1, 0.30, 0.0],
    "FM Bells": [3.5, 3.2, 7.13, 0.6, 2.20, 0.0],
    "FM Keys": [2.0, 3.0, 3.0, 0.5, 0.18, 0.25],
    "FM Glass": [5.0, 1.6, 9.0, 0.35, 1.20, 0.0],
    "Metal": [1.414, 4.0, 2.83, 1.0, 0.90, 0.35],
    "FM Pad": [1.003, 2.4, 0.5, 0.45, 0.0, 0.12],
}

TEX_TYPES = []  # (name, short, family, index into family table)
for n, s in zip(STRING_TYPES, ["Str", "Vint", "Bras", "Orgn", "Chor"]):
    TEX_TYPES.append((n, s, 0, list(STRING_TYPES).index(n)))
for (n, _, _), s in zip(WAVETABLES, ["PSaw", "Sqr", "Draw", "Reed", "Reso", "Malt", "Bell", "Add", "Vox", "Glas"]):
    TEX_TYPES.append((n, s, 1, [w[0] for w in WAVETABLES].index(n)))
for i, c in enumerate(CHORDS):
    TEX_TYPES.append((c[0], c[1], 1, len(WAVETABLES) + i))
for n, s in zip(FM_TYPES, ["EP", "FMBl", "Keys", "FMGl", "Metl", "FMPd"]):
    TEX_TYPES.append((n, s, 2, list(FM_TYPES).index(n)))

# Amino wavetables (MIT, Amino contributors; tables 32-47 and 68-73 derive from AKWF-FREE, CC0): 74 tables
# of 64 waves x 128 signed 8-bit samples, extracted from Amino's dsp.so (tools/data/). They are appended
# as wavetable-family types: the engine band-limits them into mip levels when a table is selected.
AM_DIR = os.path.join(HERE, "data")
AMINO_RAW = open(os.path.join(AM_DIR, "amino_wt.bin"), "rb").read()
AMINO_NAMES = json.load(open(os.path.join(AM_DIR, "amino_wt_names.json")))
AM_SRC_WAVES, AM_LEN = 64, 128
assert len(AMINO_RAW) == len(AMINO_NAMES) * AM_SRC_WAVES * AM_LEN
# Every Amino table ends in the same three utility waves (61 triangle, 62 square, 63 saw: the old
# wavetable-synth trick of parking basic shapes at the end of each table). They are byte-identical across all
# 74 tables and a hard jump from wave 60, so they are cut: Variation then scans only the real 61 waves.
AM_WAVES = 61
for _k in range(AM_WAVES, AM_SRC_WAVES):
    _ref = AMINO_RAW[_k * AM_LEN:(_k + 1) * AM_LEN]
    assert all(AMINO_RAW[(t * AM_SRC_WAVES + _k) * AM_LEN:(t * AM_SRC_WAVES + _k + 1) * AM_LEN] == _ref
               for t in range(len(AMINO_NAMES))), "Amino utility tail moved"


def _abbrev(name, used):
    s = re.sub(r"[^A-Za-z0-9+]", "", name)
    cands = []
    if len(s) <= 4:
        cands.append(s)
    if name.split()[-1].isdigit():
        cands.append(s[:3] + name.split()[-1])
    cons = s[0] + re.sub(r"[aeiouAEIOU]", "", s[1:])
    cands += [cons[:4] if len(cons) >= 4 else s[:4], s[:4], cons[:3] + s[-1], s[:3] + s[-1], s[:2] + s[-2:]]
    for c in cands:
        if c and c not in used:
            return c
    for i in range(2, 99):
        c = s[:3 - len(str(i)) + 1] + str(i)
        if c not in used:
            return c
    raise ValueError(name)


NUM_WT_QUARK = len(WAVETABLES) + len(CHORDS)
_used_names = set(t[0] for t in TEX_TYPES)
_used_short = set(t[1] for t in TEX_TYPES)
AMINO_TYPES = []
for i, n in enumerate(AMINO_NAMES):
    disp = n + " WT" if n in _used_names else n
    sh = n[0] + re.sub(r"[aeiou]", "", n[1:])[:1] + "WT" if disp != n else _abbrev(n, _used_short)
    _used_names.add(disp)
    _used_short.add(sh)
    AMINO_TYPES.append((disp, sh))
    TEX_TYPES.append((disp, sh, 1, NUM_WT_QUARK + i))
# every wavetable, in Type order: (name, short, wave-bank index)
ALL_WT = [(t[0], t[1], t[3]) for t in TEX_TYPES if t[2] == 1]

# ------------------------------------------------------------------------------------------------
# Option lists
# ------------------------------------------------------------------------------------------------
# Mono shapes: the four analog shapes, then every wavetable (Variation scans it, Wave Env moves it)
_VA, _VA_S = ["Saw", "Pulse", "Saw+Pulse", "Sync"], ["Saw", "Puls", "S+P", "Sync"]
LINE_SHAPES = _VA + [w[0] + " WT" if w[0] in _VA else w[0] for w in ALL_WT]
LINE_SHAPES_S = _VA_S + [w[0][0] + w[0][-1] + "WT" if w[0] in _VA else w[1] for w in ALL_WT]
assert len(set(LINE_SHAPES)) == len(LINE_SHAPES) and len(set(LINE_SHAPES_S)) == len(LINE_SHAPES_S)
LINE_VA = 4
ENV_MODES = ["Trigger", "Sustain"]
ENV_MODES_S = ["Trig", "Sus"]
LINE_MODES = ["Mono", "Legato", "Poly"]
LINE_MODES_S = ["Mono", "Leg", "Poly"]
# Pads filter. Morph: Resonance becomes the morph (LP > BP > HP) at a fixed mild resonance.
# Comb+ peaks on every harmonic of the cutoff, Comb- on the odd harmonics of half of it.
FILTER_TYPES = ["Off", "LP12", "LP24", "HP12", "BP12", "BP24", "Dual BP", "Notch", "Comb+", "Comb-", "Morph"]
FILTER_TYPES_S = ["Off", "LP12", "LP24", "HP12", "BP12", "BP24", "2BP", "Ntch", "Cmb+", "Cmb-", "Mrph"]
FX_TYPES = ["Chorus", "Chorus 2", "Flanger", "Flanger 2", "Phaser", "Phaser 2", "Tremolo", "Drive",
            "Delay", "Ping Pong", "Tape Echo"]
FX_TYPES_S = ["Cho", "Cho2", "Flng", "Flg2", "Phas", "Phs2", "Trem", "Drv", "Dly", "Ping", "Tape"]
SHIM_PITCH = ["-12", "-7", "-5", "+5", "+7", "+12", "+19", "+24"]
SHIM_SEMIS = [-12, -7, -5, 5, 7, 12, 19, 24]
ENS_TYPES = ["Strings", "Chorus", "Both", "Deep"]
ENS_TYPES_S = ["Str", "Cho", "Both", "Deep"]
DRIVE_POS = ["Pre Filter", "Post Filter", "Amp"]
DRIVE_POS_S = ["Pre", "Post", "Amp"]
LFO_SHAPES = ["Sine", "Tri", "Saw", "Square", "S&H", "Random"]
LFO_SHAPES_S = ["Sine", "Tri", "Saw", "Sqr", "S&H", "Rnd"]
ANIM_DEST = ["Variation", "Pitch", "Filter", "Pan", "Level"]
ANIM_DEST_S = ["Var", "Ptch", "Filt", "Pan", "Lvl"]
ROUTES = ["FX+Rev", "Rev", "Dry"]
ROUTES_S = ["All", "Rev", "Dry"]
# Arp types. Leap In / Leap Out, Pattern, the octave-range set, the 40 progression patterns, the 40
# rhythms and the Drop / velocity / gate / octave / note variation follow Super Arp
# (handcraftedcc/schwung-superarp, MIT); see THIRD_PARTY_NOTICES.md.
ARP_MODES = ["Off", "Up", "Down", "Up/Down", "Down/Up", "Random", "Order", "Chord", "Leap In", "Leap Out", "Pattern"]
ARP_MODES_S = ["Off", "Up", "Down", "UpDn", "DnUp", "Rnd", "Ord", "Chrd", "LpIn", "LpOt", "Patt"]
ARP_OCTS = ["0", "+1", "+2", "+3", "-1", "-2", "-3", "-1..+1", "-2..+2"]
ARP_OCTS_S = ["0", "+1", "+2", "+3", "-1", "-2", "-3", "+-1", "+-2"]
ARP_OCT_SEQ = [[0], [0, 1], [0, 1, 2], [0, 1, 2, 3], [0, -1], [0, -1, -2], [0, -1, -2, -3], [-1, 0, 1], [-2, -1, 0, 1, 2]]
# progression patterns: numbers = held notes by pitch (1 = lowest), "-" = next step, "(..)" = a chord step
ARP_PATTERNS = ["1-3-2-4", "1-2-4-3", "1-4-2-3", "2-4-1-3", "1-3-4-2", "1-3-2-4-3-5", "1-2-3-2-4-3", "1-4-2-3-5-3", "2-3-1-4-2-5", "1-2-4-3-5-2", "1-3-2-4-3-5-4-2", "1-2-3-2-1-4-3-5", "1-4-2-3-5-4-2-1", "2-3-1-4-2-5-3-1", "1-2-4-3-5-4-3-2", "(1-2)-(3-4)", "(1-3)-(2-4)", "(1-2)-(1-2-3)", "(1-3)-(1-4-5)", "(2-3)-(1-4)", "(1-2)-3-(3-4)", "(1-2)-(3-4)-2", "(1-3)-(2-4)-1", "(1-2-3)-(1-4)-2", "(2-3)-(1-4-5)-3", "(1-2)-3-(3-4)-1", "(1-2)-(3-4)-(2-3)-1", "(1-3)-2-(2-4)-3", "(1-2-3)-(2-4)-3-1", "(2-3)-(1-4)-2-(3-5)", "(1-2)-(1-2-3)-(3-4)", "(1-3)-(2-4)-(1-4-5)", "(1-2-3)-(1-4)-(2-3)", "(2-3)-(1-4-5)-(1-2)", "(1-3)-(2-4)-(3-5)", "(1-2-3)-(2-3-4)-(1-4)", "(1-2)-(2-3-4)-(1-4-5)", "(1-3-4)-(2-4)-(1-2-5)", "(2-3-5)-(1-4)-(1-2-3)", "(1-2-4)-(2-3-5)-(1-3-4)"]


def parse_pattern(t):
    steps = []
    for tok in re.findall(r"\([^)]*\)|\d+", t):
        idx = [int(x) for x in re.findall(r"\d+", tok)]
        steps.append(sum(1 << (i - 1) for i in idx))
    assert 1 <= len(steps) <= 8, t
    return steps
ARP_SOURCES = ["Keys", "Pads Chord"]
ARP_SOURCES_S = ["Keys", "Chrd"]
ARP_RATES = ["1/4", "1/8D", "1/8", "1/8T", "1/16", "1/16T", "1/32"]
ARP_RATE_BEATS = [1.0, 0.75, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125]
KEY_MODES = ["Stack", "Split", "Pads", "Mono"]
KEY_MODES_S = ["Stk", "Splt", "Pads", "Mono"]
WHEEL = ["Vibrato", "Animate", "Cutoff", "Variation"]
WHEEL_S = ["Vib", "Anim", "Cut", "Var"]
ONOFF = ["Off", "On"]
# 16 arp rhythm patterns: (bit mask, length in steps); bit i set = step i plays.
# rhythms: "0" = play the step, "x" = rest; one character per step
ARP_RHYTHM_STR = ["00", "0x", "0x0", "0xx", "00x", "00x0", "0xx0", "0x00", "000x", "0x0xx", "0xx0x", "0x00x", "00xx0", "0xxx0", "000x0", "0x0xx0", "0xx00x", "00x0xx", "0xxx00", "000xx0", "0x00x0x", "0xx0x0x", "0xx0x00", "0x0x0xx", "0xx0xx0", "00x00x0", "0xxx0x0x", "0x00x0xx", "0x0xx0xx", "00x0x00x", "0x00xx0x", "0xx00x0x", "0xxx00x0", "0x0xx0x0x0xx", "0xx0x00x0xx", "0xxx0x0xxx0", "0x0x00xx0x0x", "0x00x0xx0x00x", "0xx0x0xx0xx0x", "0xxx0xx0xxx0x"]
ARP_RHYTHMS = [(sum(1 << i for i, c in enumerate(r) if c == "0"), len(r)) for r in ARP_RHYTHM_STR]
DELAY_DIVS = ["1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2"]
DELAY_BEATS = [0.125, 1 / 6, 0.25, 1 / 3, 0.375, 0.5, 2 / 3, 0.75, 1.0, 4 / 3, 1.5, 2.0]

# ------------------------------------------------------------------------------------------------
# Parameters.  kind: f float, i int, e enum
# ------------------------------------------------------------------------------------------------
P = []


def F(key, name, short, default, mn=0.0, mx=1.0, unit=None, **kw):
    P.append(dict(key=key, name=name, short=short, kind="f", mn=mn, mx=mx, default=default, unit=unit, **kw))


def I(key, name, short, default, mn, mx, unit=None, **kw):
    P.append(dict(key=key, name=name, short=short, kind="i", mn=mn, mx=mx, default=default, unit=unit, **kw))


def E(key, name, short, default, opts, sopts=None, **kw):
    P.append(dict(key=key, name=name, short=short, kind="e", mn=0, mx=len(opts) - 1, default=default,
                  opts=opts, sopts=sopts, **kw))


CARD = "cards.js#quark_card"
ENV = lambda g, r: {"group": g, "role": r, "kind": "envelope"}

# Pads (the left panel)
E("tx_type", "Pads Type", "Type", 0, [t[0] for t in TEX_TYPES], [t[1] for t in TEX_TYPES],
  viz={"group": "txw", "role": "type", "kind": "custom:qwave"}, card=CARD)
F("tx_var", "Variation", "Var", 0.3, viz={"group": "txw", "role": "variation", "kind": "custom:qwave"}, live=True)
F("tx_anim", "Animate", "Anim", 0.25)
F("tx_depth", "Var Env", "VEnv", 0.0, -1.0, 1.0)
F("tx_a", "Pads Attack", "Atk", 0.32, viz=ENV("tx", "attack"))
F("tx_d", "Pads Decay", "Dec", 0.5, viz=ENV("tx", "decay"))
F("tx_s", "Pads Sustain", "Sus", 0.85, viz=ENV("tx", "sustain"))
F("tx_r", "Pads Release", "Rel", 0.5, viz=ENV("tx", "release"))
# Mono (the middle panel) and the mix
E("ln_shape", "Mono Shape", "Shape", 0, LINE_SHAPES, LINE_SHAPES_S,
  viz={"group": "lnw", "role": "shape", "kind": "custom:qline"}, card=CARD)
F("ln_var", "Mono Variation", "Var", 0.15, viz={"group": "lnw", "role": "variation", "kind": "custom:qline"})
F("ln_a", "Mono Attack", "Atk", 0.04, viz=ENV("ln", "attack"))
F("ln_d", "Mono Decay", "Dec", 0.42, viz=ENV("ln", "decay"))
F("ens_amt", "Pads Ensemble", "Ens", 0.65, card=CARD)
F("tx_vol", "Pads Volume", "PdVol", 0.8, viz={"kind": "fader"})
F("ln_vol", "Mono Volume", "MnVol", 0.0, viz={"kind": "fader"})
F("vintage", "Vintage", "Vint", 0.25)
# Effects (the right panel)
F("fx_ctrl", "Effect Control", "Ctrl", 0.3)
F("fx_amt", "Effect Amount", "FxAmt", 0.0)
E("sh_pitch", "Shimmer Pitch", "Pitch", 5, SHIM_PITCH, card=CARD)
F("sh_amt", "Shimmer Amount", "Shim", 0.0, card=CARD)
F("rv_decay", "Reverb Decay", "Decay", 0.5)
F("rv_amt", "Reverb Amount", "Rvb", 0.28)
E("fx_type", "Effect Type", "FxTyp", 0, FX_TYPES, FX_TYPES_S)
F("vol", "Volume", "Vol", 0.75, viz={"kind": "fader"})
# Pads tone: filter + mod env
E("flt_type", "Filter Type", "Type", 1, FILTER_TYPES, FILTER_TYPES_S)
F("flt_cut", "Cutoff", "Cut", 0.9, viz={"group": "flt", "role": "cutoff", "kind": "custom:qflt"}, live=True)
F("flt_res", "Resonance", "Res", 0.1, viz={"group": "flt", "role": "resonance", "kind": "custom:qflt"})
F("flt_env", "Filter Env", "FEnv", 0.0, -1.0, 1.0)
F("me_a", "Mod Env Attack", "Atk", 0.0, viz=ENV("me", "attack"))
F("me_d", "Mod Env Decay", "Dec", 0.5, viz=ENV("me", "decay"))
F("me_s", "Mod Env Sustain", "Sus", 0.0, viz=ENV("me", "sustain"))
F("me_r", "Mod Env Release", "Rel", 0.4, viz=ENV("me", "release"))
# Pads drive + detune (thickness)
F("tx_drive", "Pads Drive", "Drive", 0.0, viz={"group": "drv", "role": "drive", "kind": "custom:qdrive"})
E("tx_drvpos", "Drive Position", "DrvPs", 0, DRIVE_POS, DRIVE_POS_S,
  viz={"group": "drv", "role": "position", "kind": "custom:qdrive"})
F("tx_dtone", "Drive Tone", "DTone", 0.7)
F("tx_detune", "Pads Detune", "Detun", 0.0, viz={"group": "det", "role": "detune", "kind": "custom:qdet"}, card=CARD)
F("tx_bloom", "Detune Bloom", "Bloom", 0.0, viz={"group": "det", "role": "bloom", "kind": "custom:qdet"}, card=CARD)
F("tx_spread", "Spread", "Sprd", 0.3)
I("tx_voices", "Pads Voices", "Voics", 8, 2, 12)
F("flt_key", "Filter Keytrack", "KeyTk", 0.5)
# Motion
E("an_shape", "Animate Shape", "Shape", 0, LFO_SHAPES, LFO_SHAPES_S, viz={"group": "an", "role": "shape", "kind": "lfo"})
F("an_rate", "Animate Rate", "Rate", 0.35, viz={"group": "an", "role": "rate", "kind": "lfo"})
E("an_dest", "Animate Target", "Dest", 0, ANIM_DEST, ANIM_DEST_S)
F("vib", "Vibrato", "Vib", 0.0)
F("vib_rate", "Vibrato Rate", "VRate", 0.45)
E("ens_type", "Ensemble Type", "EnsTp", 0, ENS_TYPES, ENS_TYPES_S, card=CARD)
# Mono tone
F("ln_cut", "Mono Cutoff", "Cut", 0.75, viz={"group": "lf", "role": "cutoff", "kind": "filter"})
F("ln_res", "Mono Resonance", "Res", 0.15, viz={"group": "lf", "role": "resonance", "kind": "filter"})
F("ln_fenv", "Mono Filter Env", "FEnv", 0.3, -1.0, 1.0)
F("ln_wenv", "Mono Wave Env", "WvEnv", 0.0, -1.0, 1.0)
F("ln_det", "Mono Detune", "Detun", 0.22)
I("ln_int", "Mono Interval", "Intvl", 0, -24, 24, unit="st")
F("ln_glide", "Mono Glide", "Glide", 0.0)
E("ln_sus", "Mono Envelope", "EnvMd", 1, ENV_MODES, ENV_MODES_S)
# Mono arp
E("arp_mode", "Arp Type", "Type", 0, ARP_MODES, ARP_MODES_S, card=CARD)
E("arp_src", "Arp Source", "Src", 1, ARP_SOURCES, ARP_SOURCES_S, card=CARD)
E("arp_rate", "Arp Rate", "Rate", 4, ARP_RATES)
E("arp_oct", "Arp Octaves", "Oct", 0, ARP_OCTS, ARP_OCTS_S, card=CARD)
F("arp_gate", "Arp Gate", "Gate", 0.55, card=CARD)
F("arp_swing", "Arp Swing", "Swing", 0.0, card=CARD)
E("arp_hold", "Arp Latch", "Latch", 0, ONOFF, viz={"kind": "switch"})
I("arp_rhy", "Arp Rhythm", "Rhym", 0, 0, len(ARP_RHYTHMS) - 1, card=CARD)
# arp variation
I("arp_pat", "Arp Pattern", "Patt", 0, 0, len(ARP_PATTERNS) - 1, card=CARD)
F("arp_drop", "Arp Drop", "Drop", 0.0, card=CARD)
I("arp_vel", "Arp Velocity", "Vel", 0, 0, 127, card=CARD)
F("arp_vrnd", "Arp Vel Random", "VRnd", 0.0, card=CARD)
F("arp_grnd", "Arp Gate Random", "GRnd", 0.0, card=CARD)
F("arp_ornd", "Arp Oct Random", "ORnd", 0.0, card=CARD)
F("arp_nrnd", "Arp Note Random", "NRnd", 0.0, card=CARD)
I("arp_loop", "Arp Var Loop", "Loop", 0, 0, 32, card=CARD)
# Space detail
F("fx_tone", "Effect Control 2", "Ctrl2", 0.4)
F("rv_size", "Reverb Size", "Size", 0.6)
F("rv_tone", "Reverb Tone", "Tone", 0.55)
E("tx_fx", "Pads FX Route", "PdFX", 0, ROUTES, ROUTES_S)
E("ln_fx", "Mono FX Route", "MnFX", 0, ROUTES, ROUTES_S)
# Mono space: < 0 random place per note, > 0 fixed slots stepped per note (Poly: per voice)
F("ln_spread", "Mono Spread", "Wide", 0.0, -1.0, 1.0)
F("ln_dly", "Mono Delay", "Delay", 0.0)
E("ln_dtime", "Mono Delay Time", "Time", 7, DELAY_DIVS, [d.replace("1/", "") for d in DELAY_DIVS])
# Play
I("tx_oct", "Pads Octave", "PdOct", 0, -2, 2, unit="oct")
I("ln_oct", "Mono Octave", "MnOct", 0, -2, 2, unit="oct")
E("key_mode", "Key Mode", "Keys", 0, KEY_MODES, KEY_MODES_S)
I("split", "Split Point", "Split", 60, 24, 96, card=CARD)
F("vel", "Velocity", "Vel", 0.5)
E("wheel", "Mod Wheel", "Wheel", 0, WHEEL, WHEEL_S)
E("ln_mode", "Mono Mode", "Mode", 1, LINE_MODES, LINE_MODES_S)
E("tx_latch", "Pads Latch", "Latch", 0, ONOFF, viz={"kind": "switch"})

KEYS = [p["key"] for p in P]
assert len(KEYS) == len(set(KEYS)), "duplicate key"
BY_KEY = {p["key"]: p for p in P}
for p in P:
    assert len(p["short"]) <= 5, p["key"]
    if p["kind"] == "e":
        so = p.get("sopts") or p["opts"]
        for o in so:
            assert len(o) <= 4 or p.get("sopts") is None, (p["key"], o)

# ------------------------------------------------------------------------------------------------
# Pages (the jog walk).  The first three are the panel; the rest hold the depth.
# ------------------------------------------------------------------------------------------------
# The host shows a level's preset browser before its knobs, so Presets come first, then every Pads
# page, every Mono page, and the shared mix / effects / play pages last. Cutoff and Resonance sit on
# knobs 1-2 on both filter pages, Filter Env on 4. None leaves a knob empty (keeps a row together).
PAGES = [
    ("root", "Pads", ["tx_type", "tx_var", "tx_anim", "tx_depth", "tx_a", "tx_d", "tx_s", "tx_r"]),
    ("ptone", "Pads Filter", ["flt_cut", "flt_res", "flt_type", "flt_env", "me_a", "me_d", "me_s", "me_r"]),
    ("pdrive", "Pads Drive", ["tx_drive", "tx_drvpos", "tx_dtone", "tx_spread", "tx_detune", "tx_bloom", "tx_voices", "flt_key"]),
    ("pmotion", "Pads Motion", ["an_shape", "an_rate", "tx_anim", "an_dest", "ens_amt", "ens_type", None, "tx_latch"]),
    ("mono", "Mono", ["ln_shape", "ln_var", "ln_det", "ln_int", "ln_a", "ln_d", "ln_sus", "ln_mode"]),
    ("mtone", "Mono Filter", ["ln_cut", "ln_res", "ln_wenv", "ln_fenv", "ln_glide", "ln_spread", "ln_dly", "ln_dtime"]),
    ("arp", "Mono Arp", ["arp_mode", "arp_src", "arp_rate", "arp_oct", "arp_gate", "arp_swing", "arp_hold", "arp_rhy"]),
    ("arp2", "Arp Variation", ["arp_pat", "arp_drop", "arp_vel", "arp_vrnd", "arp_grnd", "arp_ornd", "arp_nrnd", "arp_loop"]),
    ("mix", "Mix & Effect", ["tx_vol", "ln_vol", "tx_fx", "ln_fx", "fx_type", "fx_ctrl", "fx_tone", "fx_amt"]),
    ("space", "Reverb", ["rv_decay", "rv_size", "rv_tone", "rv_amt", "sh_pitch", "sh_amt", "vintage", "vol"]),
    ("play", "Play", ["tx_oct", "ln_oct", "key_mode", "split", "vel", "wheel", "vib", "vib_rate"]),
]
_on_page = [k for _, _, ks in PAGES for k in ks if k]
assert set(_on_page) == set(KEYS), sorted(set(KEYS) ^ set(_on_page))

# ------------------------------------------------------------------------------------------------
# Factory bank.  Each preset overrides defaults; everything else is the default above.
# ------------------------------------------------------------------------------------------------
T = {t[0]: i for i, t in enumerate(TEX_TYPES)}
FX = {n: i for i, n in enumerate(FX_TYPES)}
PRESETS = [
    ("Init Strings", {}),
    ("Ensemble Strings", dict(tx_type=T["Strings"], tx_var=0.12, tx_anim=0.18, an_rate=0.3, ens_amt=0.85, ens_type=0,
                         tx_a=0.35, tx_r=0.55, rv_amt=0.3, rv_decay=0.55, vintage=0.3)),
    ("Cello Section", dict(tx_type=T["Strings"], tx_var=0.42, tx_anim=0.1, ens_amt=0.7, tx_a=0.38, tx_r=0.5,
                           tx_oct=-1, rv_amt=0.25, flt_cut=0.8)),
    ("Vintage Dream", dict(tx_type=T["Vintage"], tx_var=0.55, tx_anim=0.3, an_rate=0.22, ens_amt=1.0, ens_type=2,
                          tx_a=0.42, tx_r=0.6, rv_amt=0.4, rv_decay=0.7, sh_amt=0.15)),
    ("Brass Swell", dict(tx_type=T["Brass"], tx_var=0.5, tx_depth=0.0, me_a=0.32, me_d=0.55, me_s=0.4, me_r=0.4,
                         tx_a=0.28, tx_r=0.35, ens_amt=0.4, rv_amt=0.22, fx_type=FX["Chorus"], fx_amt=0.2)),
    ("Combo Organ", dict(tx_type=T["Organ"], tx_var=0.45, tx_a=0.05, tx_d=0.3, tx_s=1.0, tx_r=0.12, ens_amt=0.5,
                         ens_type=1, fx_type=FX["Phaser"], fx_amt=0.45, fx_ctrl=0.35, rv_amt=0.2, vib=0.15)),
    ("Ahh Choir", dict(tx_type=T["Choir"], tx_var=0.5, tx_anim=0.3, an_rate=0.18, ens_amt=0.9, tx_a=0.45,
                       tx_r=0.6, rv_amt=0.45, rv_decay=0.75, sh_amt=0.1)),
    ("Shimmer Glass", dict(tx_type=T["Glass"], tx_var=0.65, tx_anim=0.35, an_rate=0.2, tx_depth=0.0, tx_a=0.4,
                           tx_r=0.65, ens_amt=0.6, sh_amt=0.55, sh_pitch=5, rv_amt=0.5, rv_decay=0.82,
                           flt_cut=0.85)),
    ("Poly Saw Pad", dict(tx_type=T["Poly Saw"], tx_var=0.4, tx_anim=0.25, tx_spread=0.55, flt_type=2,
                          flt_cut=0.6, flt_res=0.2, flt_env=0.25, me_a=0.4, me_d=0.6, me_s=0.4, tx_a=0.35,
                          tx_r=0.5, ens_amt=0.7, ens_type=2, rv_amt=0.35)),
    ("Resonant Sweep", dict(tx_type=T["Resonant"], tx_var=0.1, tx_depth=0.6, me_a=0.5, me_d=0.7, me_s=0.3,
                            me_r=0.5, tx_anim=0.15, an_shape=1, an_rate=0.15, tx_a=0.3, tx_r=0.5, ens_amt=0.5,
                            fx_type=FX["Delay"], fx_amt=0.25, fx_ctrl=0.6, fx_tone=0.4, rv_amt=0.3)),
    ("Vocal Morph", dict(tx_type=T["Vocal"], tx_var=0.3, tx_anim=0.55, an_rate=0.12, an_shape=1, ens_amt=0.7,
                         tx_a=0.38, tx_r=0.55, rv_amt=0.4, rv_decay=0.7)),
    ("Mallet Keys", dict(tx_type=T["Mallet"], tx_var=0.45, tx_depth=-0.3, me_a=0.0, me_d=0.35, me_s=0.0,
                         tx_a=0.0, tx_d=0.45, tx_s=0.0, tx_r=0.42, ens_amt=0.35, ens_type=1, rv_amt=0.3,
                         fx_type=FX["Ping Pong"], fx_amt=0.2, fx_ctrl=0.55)),
    ("Bell Garden", dict(tx_type=T["Bellish"], tx_var=0.55, tx_anim=0.2, tx_a=0.0, tx_d=0.6, tx_s=0.25,
                         tx_r=0.6, ens_amt=0.5, sh_amt=0.35, rv_amt=0.45, rv_decay=0.8)),
    ("Drawbar Wash", dict(tx_type=T["Drawbar"], tx_var=0.6, tx_anim=0.2, an_shape=1, tx_a=0.15, tx_r=0.3,
                          ens_amt=0.8, ens_type=2, fx_type=FX["Chorus 2"], fx_amt=0.3, rv_amt=0.25)),
    ("Square Choir", dict(tx_type=T["Square"], tx_var=0.35, tx_anim=0.4, an_rate=0.25, flt_type=1, flt_cut=0.65,
                          tx_a=0.35, tx_r=0.5, ens_amt=0.75, rv_amt=0.35)),
    ("Tine Piano", dict(tx_type=T["E.Piano"], tx_var=0.35, tx_depth=0.35, me_a=0.0, me_d=0.45, me_s=0.1, me_r=0.3,
                        tx_a=0.0, tx_d=0.62, tx_s=0.3, tx_r=0.35, ens_amt=0.3, ens_type=1, fx_type=FX["Tremolo"],
                        fx_amt=0.35, fx_ctrl=0.45, rv_amt=0.22, vel=0.75)),
    ("FM Bells", dict(tx_type=T["FM Bells"], tx_var=0.35, tx_depth=0.3, me_d=0.7, tx_a=0.0, tx_d=0.7, tx_s=0.0,
                      tx_r=0.65, ens_amt=0.4, rv_amt=0.45, rv_decay=0.75, sh_amt=0.25)),
    ("Metal Drone", dict(tx_type=T["Metal"], tx_var=0.3, tx_anim=0.5, an_shape=5, an_rate=0.2, tx_a=0.5,
                         tx_r=0.7, ens_amt=0.6, flt_type=2, flt_cut=0.55, flt_res=0.35, rv_amt=0.5,
                         rv_decay=0.85, fx_type=FX["Phaser 2"], fx_amt=0.4, fx_ctrl=0.15)),
    ("FM Pad Air", dict(tx_type=T["FM Pad"], tx_var=0.35, tx_anim=0.3, an_rate=0.15, tx_a=0.45, tx_r=0.6,
                        ens_amt=0.8, ens_type=2, rv_amt=0.4, sh_amt=0.2)),
    ("Strings + Bass", dict(tx_type=T["Strings"], tx_var=0.2, ens_amt=0.85, tx_a=0.38, tx_r=0.5, ln_vol=0.7,
                            ln_shape=0, ln_var=0.0, ln_oct=-1, key_mode=1, split=55, ln_cut=0.5, ln_res=0.25,
                            ln_fenv=0.35, ln_a=0.0, ln_d=0.35, ln_sus=0, ln_mode=1, rv_amt=0.25)),
    ("Mono Lead", dict(tx_vol=0.0, ln_vol=0.8, ln_shape=2, ln_var=0.7, ln_det=0.3, ln_cut=0.7, ln_res=0.2,
                        ln_fenv=0.2, ln_a=0.08, ln_d=0.5, ln_glide=0.25, ln_mode=1, key_mode=3,
                        fx_type=FX["Delay"], fx_amt=0.3, fx_ctrl=0.6, rv_amt=0.3)),
    ("Sync Arp", dict(tx_type=T["Vintage"], tx_var=0.3, tx_vol=0.5, ens_amt=0.8, tx_a=0.4, tx_r=0.5, ln_vol=0.65,
                      ln_shape=3, ln_var=0.0, ln_int=12, ln_cut=0.65, ln_res=0.3, ln_fenv=0.4, ln_a=0.0,
                      ln_d=0.3, ln_sus=0, ln_mode=2, arp_mode=1, arp_oct=1, arp_rate=4,
                      fx_type=FX["Ping Pong"], fx_amt=0.25, fx_ctrl=0.55, rv_amt=0.3)),
    ("Pulse Bass", dict(tx_vol=0.0, ln_vol=0.85, ln_shape=1, ln_var=0.2, ln_oct=-1, ln_cut=0.45, ln_res=0.3,
                        ln_fenv=0.45, ln_a=0.0, ln_d=0.3, ln_sus=1, ln_mode=1, key_mode=3, rv_amt=0.1,
                        ln_fx=1)),
    ("Layered Dream", dict(tx_type=T["Strings"], tx_var=0.65, tx_anim=0.2, ens_amt=0.8, ens_type=2, tx_a=0.4,
                          tx_r=0.55, ln_vol=0.45, ln_shape=1, ln_var=0.55, ln_int=12, ln_a=0.25, ln_d=0.55,
                          ln_sus=1, ln_mode=2, ln_cut=0.6, rv_amt=0.4, rv_decay=0.7, sh_amt=0.2)),
]
LS = {n: i for i, n in enumerate(LINE_SHAPES)}
PRESETS += [
    ("Driven Drone", dict(vol=0.66, tx_type=T["Poly Saw"], tx_var=0.5, tx_anim=0.2, an_rate=0.15, tx_drive=0.6, tx_drvpos=2,
                          tx_dtone=0.55, tx_detune=0.55, tx_bloom=0.6, ens_type=3, ens_amt=0.8, tx_a=0.5, tx_r=0.7,
                          tx_latch=1, flt_type=2, flt_cut=0.7, flt_res=0.15, rv_amt=0.4, rv_decay=0.8, sh_amt=0.2)),
    ("Fuzz Strings", dict(tx_type=T["Strings"], tx_var=0.3, tx_drive=0.45, tx_drvpos=0, tx_dtone=0.6, flt_type=2,
                          flt_cut=0.62, flt_res=0.25, tx_detune=0.25, ens_amt=0.75, tx_a=0.3, tx_r=0.45, rv_amt=0.3)),
    ("Chord Pluck Arp", dict(tx_type=T["Vintage"], tx_var=0.35, tx_vol=0.55, ens_amt=0.8, tx_a=0.45, tx_r=0.6,
                             ln_vol=0.6, ln_shape=LS["Pluck"], ln_var=0.15, ln_wenv=0.71, ln_det=0.08, ln_cut=0.72,
                             ln_res=0.2, ln_fenv=0.35, ln_a=0.0, ln_d=0.32, ln_sus=0, ln_mode=2, ln_oct=1, key_mode=2,
                             arp_mode=3, arp_src=1, arp_oct=1, arp_rate=4, arp_drop=0.15, arp_vrnd=0.3, fx_type=FX["Ping Pong"], fx_amt=0.25,
                             fx_ctrl=0.55, rv_amt=0.35)),
    ("Scanned Glass", dict(tx_type=T["Scanned"], tx_var=0.2, tx_anim=0.45, an_rate=0.18, an_shape=1, tx_spread=0.5,
                           tx_a=0.4, tx_r=0.6, ens_amt=0.6, ens_type=2, sh_amt=0.3, rv_amt=0.45, rv_decay=0.75)),
    ("Bell Arp Keys", dict(tx_type=T["Bell"], tx_var=0.3, tx_depth=-0.3, me_d=0.4, tx_a=0.0, tx_d=0.55, tx_s=0.2,
                           tx_r=0.5, ens_amt=0.4, ln_vol=0.5, ln_shape=LS["Mallet WT"], ln_var=0.2, ln_wenv=0.56,
                           ln_a=0.0, ln_d=0.28, ln_sus=0, ln_mode=2, ln_oct=1, ln_cut=0.85, ln_fenv=0.2, arp_mode=10,
                           arp_src=1, arp_rate=2, arp_oct=7, arp_pat=17, arp_rhy=6, arp_ornd=0.2, arp_loop=16, rv_amt=0.4, fx_type=FX["Delay"], fx_amt=0.2)),
    ("Vosim Bloom", dict(tx_type=T["Vosim"], tx_var=0.4, tx_anim=0.3, an_rate=0.1, tx_detune=0.7, tx_bloom=0.8,
                         ens_type=3, ens_amt=0.9, tx_a=0.6, tx_r=0.75, tx_latch=1, flt_type=1, flt_cut=0.75,
                         rv_amt=0.5, rv_decay=0.85, sh_amt=0.25, sh_pitch=5)),
    ("Grit Bass", dict(tx_vol=0.0, ln_vol=0.85, ln_shape=LS["Crushed"], ln_var=0.3, ln_wenv=-1.0, ln_oct=-1,
                       ln_cut=0.5, ln_res=0.3, ln_fenv=0.4, ln_a=0.0, ln_d=0.35, ln_sus=1, ln_mode=1, key_mode=3,
                       ln_det=0.1, rv_amt=0.1, ln_fx=1)),
    # chord stacks: one key, a whole voicing (Variation picks it)
    ("Rompler Fifths", dict(tx_type=T["Fifths"], tx_var=0.57, tx_anim=0.1, an_dest=2, an_rate=0.15, ens_amt=0.6,
                            tx_a=0.3, tx_r=0.6, flt_type=2, flt_cut=0.72, flt_res=0.15, fx_type=FX["Chorus"],
                            fx_amt=0.25, rv_amt=0.4, rv_decay=0.7, vintage=0.25)),
    ("Liquid Minor 9", dict(tx_type=T["Minor 9"], tx_var=0.43, tx_anim=0.25, an_dest=2, an_shape=1, an_rate=0.12,
                            flt_type=2, flt_cut=0.62, flt_res=0.25, flt_env=0.15, me_a=0.4, me_d=0.6, me_s=0.3,
                            tx_a=0.25, tx_r=0.55, ens_amt=0.8, ens_type=2, tx_detune=0.25, rv_amt=0.45,
                            rv_decay=0.75, sh_amt=0.1, vintage=0.3)),
    ("Quartal Air", dict(tx_type=T["Fourths"], tx_var=0.57, tx_anim=0.12, an_dest=2, an_rate=0.08, tx_a=0.5, tx_r=0.7,
                         tx_detune=0.35, tx_bloom=0.4, ens_type=3, ens_amt=0.7, flt_cut=0.7, rv_amt=0.5,
                         rv_decay=0.85, sh_amt=0.2, sh_pitch=5)),
    ("Dub Chord Stab", dict(tx_type=T["Minor 9"], tx_var=0.57, tx_anim=0.0, tx_a=0.0, tx_d=0.3, tx_s=0.0, tx_r=0.3,
                            flt_type=2, flt_cut=0.42, flt_res=0.3, flt_env=0.45, me_a=0.0, me_d=0.35, me_s=0.0,
                            ens_amt=0.3, fx_type=FX["Delay"], fx_amt=0.35, fx_ctrl=0.5, rv_amt=0.3, rv_decay=0.6)),
    ("Major 9 Keys", dict(tx_type=T["Major 9"], tx_var=0.43, tx_anim=0.0, tx_a=0.02, tx_d=0.55, tx_s=0.5, tx_r=0.4,
                          flt_type=2, flt_cut=0.6, flt_env=0.3, me_a=0.0, me_d=0.5, me_s=0.2, ens_amt=0.5,
                          ens_type=1, rv_amt=0.3, vintage=0.3)),
]
for name, d in PRESETS:
    for k in d:
        assert k in BY_KEY, (name, k)


# ------------------------------------------------------------------------------------------------
# Emitters
# ------------------------------------------------------------------------------------------------
def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cfloat(x):
    s = repr(float(x))
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def c_chunks(js, name):
    out = ["const char %s[] =" % name]
    esc = js.replace("\\", "\\\\").replace('"', '\\"')
    for i in range(0, len(esc), 110):
        piece = esc[i:i + 110]
        # never split an escape sequence
        out.append('    "%s"' % piece)
    out[-1] += ";"
    return "\n".join(out)


def safe_chunks(js, name):
    # split on raw characters, escaping each chunk independently so escapes never straddle
    out = ["const char %s[] =" % name]
    for i in range(0, len(js), 100):
        piece = js[i:i + 100].replace("\\", "\\\\").replace('"', '\\"')
        out.append('    "%s"' % piece)
    out[-1] += ";"
    return "\n".join(out)


def enum_name(key):
    return "P_" + key.upper()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def gen_params():
    h = ["/* GENERATED by tools/gen.py. Do not edit. */", "#ifndef QUARK_PARAMS_GEN_H", "#define QUARK_PARAMS_GEN_H",
         "", "typedef struct {", "    const char *key;", "    char kind;          /* 'f' float, 'i' int, 'e' enum */",
         "    float mn, mx, def;", "    int nopts;", "    const char *const *opts;", "} QParam;", "", "enum {"]
    for p in P:
        h.append("    %s," % enum_name(p["key"]))
    h += ["    P_COUNT", "};", "", "extern const QParam quark_params[P_COUNT];",
          "extern const int quark_preset_count;", "extern const char *const quark_preset_names[];",
          "extern const float quark_presets[][P_COUNT];", "extern const char quark_chain_params[];",
          "extern const char quark_ui_hierarchy[];", ""]
    h.append("#define Q_NUM_TEX_TYPES %d" % len(TEX_TYPES))
    h.append("#define Q_NUM_ARP_RHYTHMS %d" % len(ARP_RHYTHMS))
    h.append("#define Q_NUM_ARP_PATTERNS %d" % len(ARP_PATTERNS))
    h.append("#define Q_NUM_ARP_OCTS %d" % len(ARP_OCT_SEQ))
    h.append("#define Q_NUM_WT_QUARK %d" % NUM_WT_QUARK)
    h.append("#define Q_NUM_WT_AMINO %d" % len(AMINO_NAMES))
    h.append("#define Q_NUM_WT (Q_NUM_WT_QUARK + Q_NUM_WT_AMINO)")
    h.append("#define Q_AM_WAVES %d" % AM_WAVES)
    h.append("#define Q_AM_LEN %d" % AM_LEN)
    h.append("#define Q_LINE_VA %d" % LINE_VA)
    h.append("#define Q_NUM_LINE_SHAPES %d" % len(LINE_SHAPES))
    h.append("#define Q_NUM_ENS_TYPES %d" % len(ENS_TYPES))
    h.append("#define Q_NUM_FILTER_TYPES %d" % len(FILTER_TYPES))
    h.append("extern const signed char quark_amino_wt[Q_NUM_WT_AMINO][Q_AM_WAVES][Q_AM_LEN];")
    h += ["", "#endif", ""]
    write(os.path.join(DSP, "params_gen.h"), "\n".join(h))

    c = ["/* GENERATED by tools/gen.py. Do not edit. */", '#include "params_gen.h"', ""]
    for p in P:
        if p["kind"] == "e":
            c.append("static const char *const O_%s[] = {%s};" % (p["key"], ", ".join(cstr(o) for o in p["opts"])))
    c += ["", "const QParam quark_params[P_COUNT] = {"]
    for p in P:
        opts = "O_%s" % p["key"] if p["kind"] == "e" else "0"
        n = len(p["opts"]) if p["kind"] == "e" else 0
        c.append("    {%s, '%s', %s, %s, %s, %d, %s}," % (cstr(p["key"]), p["kind"], cfloat(p["mn"]), cfloat(p["mx"]),
                                                       cfloat(p["default"]), n, opts))
    c += ["};", ""]
    write(os.path.join(DSP, "params_gen.c"), "\n".join(c))


def gen_tables():
    h = ["/* GENERATED by tools/gen.py. Do not edit. Included once, by tex.c. */", "",
         "#define Q_REG_FIELDS %d" % len(REG_FIELDS), "#define Q_WT_COUNT %d" % len(WAVETABLES),
         "#define Q_WT_FRAMES %d" % WT_FRAMES, "#define Q_WT_NH %d" % NH, ""]
    # strings
    h.append("/* string-machine register points: %s */" % " ".join(REG_FIELDS))
    allpts, starts, counts = [], [], []
    for name, pts in STRING_TYPES.items():
        starts.append(len(allpts))
        counts.append(len(pts))
        allpts += pts
    h.append("static const float Q_REGS[%d][Q_REG_FIELDS] = {" % len(allpts))
    for r in allpts:
        h.append("    {%s}," % ", ".join(cfloat(x) for x in r))
    h.append("};")
    h.append("static const unsigned char Q_STR_START[%d] = {%s};" % (len(starts), ", ".join(map(str, starts))))
    h.append("static const unsigned char Q_STR_COUNT[%d] = {%s};" % (len(counts), ", ".join(map(str, counts))))
    # fm
    h.append("/* FM recipes: %s */" % " ".join(FM_FIELDS))
    h.append("static const float Q_FM[%d][%d] = {" % (len(FM_TYPES), len(FM_FIELDS)))
    for v in FM_TYPES.values():
        h.append("    {%s}," % ", ".join(cfloat(x) for x in v))
    h.append("};")
    # texture types
    h.append("static const unsigned char Q_TEX_FAMILY[%d] = {%s};" % (len(TEX_TYPES), ", ".join(str(t[2]) for t in TEX_TYPES)))
    h.append("static const unsigned char Q_TEX_INDEX[%d] = {%s};" % (len(TEX_TYPES), ", ".join(str(t[3]) for t in TEX_TYPES)))
    # wavetable spectra, normalised per frame to peak 1 later in C
    h.append("static const unsigned char Q_WT_COS[Q_WT_COUNT] = {%s};" % ", ".join(str(w[2]) for w in WAVETABLES))
    h.append("static const float Q_WT_SPEC[Q_WT_COUNT][Q_WT_FRAMES][Q_WT_NH] = {")
    for name, frames, _ in WAVETABLES:
        h.append("  { /* %s */" % name)
        for f in frames:
            # trim trailing zeros for size
            vals = ["%.5g" % (a if abs(a) > 1e-6 else 0.0) for a in f]
            while vals and float(vals[-1]) == 0.0:
                vals.pop()
            h.append("    {%s}," % ", ".join(v + ("f" if ("." in v or "e" in v) else ".0f") for v in vals) if vals else "    {0},")
        h.append("  },")
    h.append("};")
    # chord stacks: tones as (harmonic, weight) per frame, 0 = unused
    h.append("#define Q_CH_COUNT %d" % len(CHORDS))
    h.append("#define Q_CH_TONES %d" % CH_TONES)
    h.append("static const unsigned char Q_CH_DIV[Q_CH_COUNT] = {%s};" % ", ".join(str(c[2]) for c in CHORDS))
    h.append("static const float Q_CH_TILT[Q_CH_COUNT] = {%s};" % ", ".join(cfloat(c[3]) for c in CHORDS))
    h.append("static const unsigned char Q_CH_ODD[Q_CH_COUNT] = {%s};" % ", ".join(str(c[4]) for c in CHORDS))
    th, tw = [], []
    for c in CHORDS:
        fh, fw = [], []
        for _, semis in c[5]:
            tt = chord_tones(c[2], semis) + [(0, 0.0)] * (CH_TONES - len(semis))
            fh.append("{%s}" % ", ".join(str(x[0]) for x in tt))
            fw.append("{%s}" % ", ".join(cfloat(x[1]) for x in tt))
        th.append("  {%s}, /* %s */" % (", ".join(fh), c[0]))
        tw.append("  {%s}," % ", ".join(fw))
    h.append("static const unsigned short Q_CH_H[Q_CH_COUNT][Q_WT_FRAMES][Q_CH_TONES] = {")
    h += th
    h.append("};")
    h.append("static const float Q_CH_W[Q_CH_COUNT][Q_WT_FRAMES][Q_CH_TONES] = {")
    h += tw
    h.append("};")
    h.append("static const float Q_SHIM_SEMIS[%d] = {%s};" % (len(SHIM_SEMIS), ", ".join(cfloat(s) for s in SHIM_SEMIS)))
    h.append("static const float Q_ARP_BEATS[%d] = {%s};" % (len(ARP_RATE_BEATS), ", ".join(cfloat(s) for s in ARP_RATE_BEATS)))
    h.append("static const float Q_DELAY_BEATS[%d] = {%s};" % (len(DELAY_BEATS), ", ".join(cfloat(s) for s in DELAY_BEATS)))
    h.append("static const unsigned short Q_ARP_RHY[%d][2] = {%s};" % (len(ARP_RHYTHMS), ", ".join("{%d, %d}" % r for r in ARP_RHYTHMS)))
    h.append("/* progression patterns: step count, then one bit mask of held-note indices per step */")
    h.append("static const unsigned char Q_ARP_PAT[%d][9] = {%s};" % (len(ARP_PATTERNS), ", ".join(
        "{%s}" % ", ".join(map(str, [len(st)] + st + [0] * (8 - len(st)))) for st in map(parse_pattern, ARP_PATTERNS))))
    h.append("static const signed char Q_ARP_OCT[%d][6] = {%s};" % (len(ARP_OCT_SEQ), ", ".join(
        "{%s}" % ", ".join(map(str, [len(q)] + q + [0] * (5 - len(q)))) for q in ARP_OCT_SEQ)))
    h.append("")
    write(os.path.join(DSP, "tables_gen.h"), "\n".join(h))


def num(x):
    if isinstance(x, float) and x.is_integer():
        return int(x)
    return x


def chain_params():
    out = []
    for p in P:
        e = {"key": p["key"], "name": p["name"], "short_name": p["short"]}
        if p["kind"] == "e":
            e["type"] = "enum"
            e["options"] = p["opts"]
            if p.get("sopts") and p["sopts"] != p["opts"]:
                e["short_options"] = p["sopts"]
        else:
            e["type"] = "int" if p["kind"] == "i" else "float"
            e["min"] = num(p["mn"])
            e["max"] = num(p["mx"])
            if p["kind"] == "i":
                e["step"] = 1
            else:
                e["step"] = 0.01
        e["default"] = num(p["default"])
        if p.get("unit"):
            e["unit"] = p["unit"]
        if p.get("viz"):
            e["viz"] = p["viz"]
        if p.get("live"):
            e["live"] = True
        if p.get("card"):
            e["card_script"] = p["card"]
            e["card_w"] = 104
            e["card_h"] = 40
        out.append(e)
    out.append({"key": "preset", "name": "Preset", "type": "int", "min": 0, "max": len(PRESETS) - 1, "step": 1})
    return out


def ui_hierarchy():
    levels = {}
    root_params = [{"level": level, "label": label} for level, label, _ in PAGES if level != "root"]
    for level, label, knobs in PAGES:
        lv = {"label": label, "knobs": list(knobs), "params": [k for k in knobs if k]}
        if level == "root":
            # the preset browser lives on root, so it is the first page the host plans
            lv.update({"label": "Quark", "subtitle": "Pads", "list_param": "preset", "count_param": "preset_count",
                       "name_param": "preset_name"})
            lv["params"] += root_params
        levels[level] = lv
    return {"levels": levels}


def gen_contract():
    cp = json.dumps(chain_params(), separators=(",", ":"))
    uh = json.dumps(ui_hierarchy(), separators=(",", ":"))
    c = ["/* GENERATED by tools/gen.py. Do not edit. */", '#include "params_gen.h"', "",
         safe_chunks(cp, "quark_chain_params"), "", safe_chunks(uh, "quark_ui_hierarchy"), ""]
    write(os.path.join(DSP, "contract_gen.c"), "\n".join(c))
    write(os.path.join(BUILD, "chain_params.json"), json.dumps(chain_params(), indent=1))
    write(os.path.join(BUILD, "ui_hierarchy.json"), json.dumps(ui_hierarchy(), indent=1))
    print("chain_params %d bytes, ui_hierarchy %d bytes" % (len(cp), len(uh)))


def preset_values(d):
    vals = []
    for p in P:
        v = d.get(p["key"], p["default"])
        vals.append(float(v))
    return vals


def gen_presets():
    c = ["/* GENERATED by tools/gen.py. Do not edit. */", '#include "params_gen.h"', "",
         "const int quark_preset_count = %d;" % len(PRESETS),
         "const char *const quark_preset_names[] = {%s};" % ", ".join(cstr(n) for n, _ in PRESETS),
         "const float quark_presets[][P_COUNT] = {"]
    for name, d in PRESETS:
        c.append("    { /* %s */ %s}," % (name, ", ".join(cfloat(v) for v in preset_values(d))))
    c += ["};", ""]
    write(os.path.join(DSP, "presets_gen.c"), "\n".join(c))


PREV_FRAMES, PREV_N = 8, 32


def wt_previews():
    """One base64 string per wave-bank table: 8 frames x 32 samples, int8 (UI drawing only)."""
    out = []
    for name, frames, cos in WAVETABLES:
        b = bytearray()
        for f in frames:
            xs = []
            for i in range(PREV_N):
                acc = 0.0
                for h in range(1, 25):
                    a = f[h - 1]
                    if a:
                        acc += a * (math.cos if cos else math.sin)(2 * math.pi * h * i / PREV_N)
                xs.append(acc)
            pk = max(1e-6, max(abs(x) for x in xs))
            b += bytes((int(round(x / pk * 120)) & 255) for x in xs)
        out.append(base64.b64encode(bytes(b)).decode())
    for c in CHORDS:
        # two periods of the played note (the table cycle is DIV periods, so this is a window of it)
        b = bytearray()
        for _, semis in c[5]:
            xs = [0.0] * PREV_N
            for j, (th, tw) in enumerate(chord_tones(c[2], semis)):
                phi = 2 * math.pi * ((0.618034 * j) % 1.0)
                for k in range(1, 40):
                    if (c[4] and k % 2 == 0) or k * th > 6 * c[2]:
                        continue
                    a = tw / k ** c[3]
                    for i in range(PREV_N):
                        xs[i] += a * math.sin(2 * math.pi * k * th * (2.0 * i / PREV_N) / c[2] + k * phi)
            pk = max(1e-6, max(abs(x) for x in xs))
            b += bytes((int(round(x / pk * 120)) & 255) for x in xs)
        out.append(base64.b64encode(bytes(b)).decode())
    for t in range(len(AMINO_NAMES)):
        b = bytearray()
        for k in range(PREV_FRAMES):
            w = round(k * (AM_WAVES - 1) / (PREV_FRAMES - 1))
            base = (t * AM_SRC_WAVES + w) * AM_LEN
            raw = [x - 256 if x > 127 else x for x in AMINO_RAW[base:base + AM_LEN]]
            step = AM_LEN // PREV_N
            for i in range(PREV_N):
                b.append(int(round(sum(raw[i * step:(i + 1) * step]) / step)) & 255)
        out.append(base64.b64encode(bytes(b)).decode())
    return out


def gen_amino():
    c = ["/* GENERATED by tools/gen.py from tools/data/amino_wt.bin. Do not edit.",
         " * Amino wavetables (MIT, Copyright (c) 2026 Amino contributors); tables 32-47 and 68-73 are built",
         " * from AKWF-FREE single-cycle waveforms (CC0). See THIRD_PARTY_NOTICES.md. */",
         "const signed char quark_amino_wt[%d][%d][%d] = {" % (len(AMINO_NAMES), AM_WAVES, AM_LEN)]
    for t, n in enumerate(AMINO_NAMES):
        c.append("  { /* %s */" % n)
        for w in range(AM_WAVES):
            base = (t * AM_SRC_WAVES + w) * AM_LEN
            c.append("    {%s}," % ",".join(str(x - 256 if x > 127 else x) for x in AMINO_RAW[base:base + AM_LEN]))
        c.append("  },")
    c += ["};", ""]
    write(os.path.join(DSP, "amino_gen.c"), "\n".join(c))


def gen_meta():
    meta = {
        "params": [{k: v for k, v in p.items() if k in ("key", "name", "short", "kind", "mn", "mx", "default", "opts",
                                                      "sopts", "unit", "viz", "live", "card")} for p in P],
        "pages": [{"level": "presets", "label": "Presets", "knobs": None}] +
                 [{"level": l, "label": lab, "knobs": k} for l, lab, k in PAGES],
        "presets": [{"name": n, "values": preset_values(d)} for n, d in PRESETS],
        "texTypes": [{"name": t[0], "short": t[1], "family": t[2], "index": t[3]} for t in TEX_TYPES],
        "regFields": REG_FIELDS,
        "strings": [STRING_TYPES[n] for n in STRING_TYPES],
        "fm": [FM_TYPES[n] for n in FM_TYPES],
        "wt": wt_previews(),
        "wtQuark": NUM_WT_QUARK,
        "lineVA": LINE_VA,
        "amWaves": AM_WAVES,
        "wtQuarkDense": len(WAVETABLES),
        "chordFrames": [[f[0] for f in c[5]] for c in CHORDS],
        "shimSemis": SHIM_SEMIS,
        "delayDivs": DELAY_DIVS,
        "arpRates": ARP_RATES,
        "arpRhythms": ARP_RHYTHMS,
        "arpRhythmStr": ARP_RHYTHM_STR,
        "arpPatterns": ARP_PATTERNS,
        "arpOctSeq": ARP_OCT_SEQ,
    }
    js = "/* GENERATED by tools/gen.py. Do not edit. */\nvar QUARK_META = " + json.dumps(meta, separators=(",", ":")) + ";\n"
    write(os.path.join(BUILD, "web", "meta_gen.js"), js)
    # device scripts: a compact META (no presets/pages) plus the shared helpers, pasted into each
    dev = dict(meta)
    dev.pop("presets")
    dev.pop("pages")
    dev_js = "var QUARK_META = " + json.dumps(dev, separators=(",", ":")) + ";\n"
    common = open(os.path.join(SRC, "ui", "common.js"), encoding="utf-8").read()
    for name in ("canvas.js", "cards.js"):
        t = open(os.path.join(SRC, "ui", name), encoding="utf-8").read()
        a, b = "/* BEGIN GENERATED */", "/* END GENERATED */"
        i, j = t.index(a), t.index(b)
        t = t[:i + len(a)] + "\n" + dev_js + common + "\n  " + t[j:]
        write(os.path.join(BUILD, "ui", name), t)


if __name__ == "__main__":
    gen_params()
    gen_tables()
    gen_contract()
    gen_presets()
    gen_amino()
    gen_meta()
    print("%d params, %d texture types, %d presets" % (len(P), len(TEX_TYPES), len(PRESETS)))
