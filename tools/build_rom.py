#!/usr/bin/env python3
"""Build Quark's PCM ROM from the CC0 sources (docs/QUARK_PCM_PLAN.md, sections 2.1 / 2.3).

Run this when the sources or the recipes below change; its output is committed and the normal
build (scripts/build.sh, CI) only reads it, so it never needs numpy or the raw audio:

  tools/data/rom/rom.bin    int16 little-endian samples: every zone, 3 mip levels, 3 guard samples each
  tools/data/rom/rom.json   sets (zones, roots, loops, offsets), the PCM types, UI overviews

  ~/.local/share/quark-zig/bin/python tools/build_rom.py      (numpy; raw audio in tools/data/rom_src)

Per zone: mono mix, resample to 32 kHz (windowed sinc), trim to the onset, then
  sus   attack, then a loop searched in the steady part (start + length with the best match), seam
        crossfaded (linear when the two ends correlate, equal power when they don't)
  tex   unpitched texture: a long loop with a long crossfade
  shot  one-shot (bells, keys): the attack and decay, faded out, no loop
then fine-tuned (cents, from the loop's period), and levelled per set. Mips 1 and 2 (16 / 8 kHz) are
decimated from the looped signal itself, so they wrap as cleanly as mip 0.
"""
import hashlib
import json
import math
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "data", "rom_src")
OUT = os.path.join(HERE, "data", "rom")
SR = 32000
MIPS = 3
GUARD = 3
BUDGET = 10 * 1024 * 1024        # soft: a warning above it
HARD_LIMIT = 16 * 1024 * 1024    # a mistake guard

NOTE = {n: i for i, n in enumerate("C C# D D# E F F# G G# A A# B".split())}


def midi(name):
    n = name.rstrip("-0123456789")
    return (int(name[len(n):]) + 1) * 12 + NOTE[n]


# ---- the sets -----------------------------------------------------------------------------------
# (set name, short, kind, [(file under rom_src, root note)], options)
V, VS = "VCSL/", "VSCO-2-CE/"
FS = "freesound/"
SETS = [
    ("Wine Glass", "WGls", "sus", [(V + "Idiophones/Friction Idiophones/Wine Glasses/Sustains/Slow/glass1_D#4_Slow_1_Main.wav", "D#4"),
                              (V + "Idiophones/Friction Idiophones/Wine Glasses/Sustains/Slow/glass2_F#4_Slow_1_Main.wav", "F#4"),
                              (V + "Idiophones/Friction Idiophones/Wine Glasses/Sustains/Slow/glass3_A#4_Slow_1_Main.wav", "A#4"),
                              (V + "Idiophones/Friction Idiophones/Wine Glasses/Sustains/Slow/glass4_D5_Slow_2_Main.wav", "D5")],
     dict(attack=0.9, loop=1.1)),
    ("Bowed Vibes", "BVib", "sus", [(V + "Idiophones/Struck Idiophones/Vibraphone/Bowed/Vibes_bowed_%s_rr1_Main.wav" % n, n)
                                    for n in ("G3", "D4", "A4", "E5")], dict(attack=0.6, loop=1.0)),
    ("Section Strings", "SStr", "sus", [(VS + "Strings/Cello Section/susvib/susvib_D2_v1_1.wav", "D2"),
                               (VS + "Strings/Cello Section/susvib/susvib_A2_v1_1.wav", "A2"),
                               (VS + "Strings/Viola Section/susvib/ViolaEns_susvib_F3_v1_1.wav", "F3"),
                               (VS + "Strings/Violin Section/susVib/VlnEns_susVib_C4_v1.wav", "C4"),
                               (VS + "Strings/Violin Section/susVib/VlnEns_susVib_G4_v1.wav", "G4"),
                               (VS + "Strings/Violin Section/susVib/VlnEns_susVib_D5_v1.wav", "D5")], dict(attack=0.45, loop=0.9)),
    ("Psaltery", "Psal", "sus", [(V + "Chordophones/Zithers/Psaltery, Bowed and Plucked/LongBow/BowedPsaltery_%s_Main_LongBow_rr%d.wav" % (n, r), n)
                                 for n, r in (("A#3", 2), ("E4", 2), ("A#4", 2), ("E5", 2))], dict(attack=0.4, loop=0.9)),
    ("Concert Flute", "CFlt", "sus", [(VS + "Woodwinds/Flute/susNV/LDFlute_susNV_%s_v1_1.wav" % n, n) for n in ("C4", "E4", "A4", "E5")],
     dict(attack=0.3, loop=0.9)),
    ("Ocarina", "Ocar", "sus", [(V + "Aerophones/Edge-blown Aerophones/Ocarina, Typical/Sustains/Sus/StdOcarina_Sus_%s.wav" % n, n)
                                for n in ("B3", "E4", "G#4", "C#5")], dict(attack=0.3, loop=0.9)),
    ("Organ Blower", "Blwr", "tex", [(VS + "Keys/Organ/Rode_Blower.wav", "C4")], dict(start=4.0, loop=2.2)),
    ("Ocean Drum", "Ocn", "tex", [(V + "Membranophones/Other Membranophones/Ocean Drum/OceanDrum_Sus_2_Mid.wav", "C4")], dict(start=2.0, loop=2.4)),
    ("Cymbal Bow", "Cymb", "tex", [(VS + "Percussion/susCymb1-bow-1.wav", "C4")], dict(start=1.0, loop=2.2)),
    ("Gong Scrape", "Gong", "tex", [(VS + "Percussion/gongscrape_pp.wav", "C4")], dict(start=0.6, loop=2.0)),
    ("Hand Chimes", "Chim", "shot", [(V + "Idiophones/Struck Idiophones/Hand Chimes/sus_%s_r01_main.wav" % n, n) for n in ("D4", "A4", "F#5")],
     dict(length=2.0)),
    ("Soft Vibes", "SVib", "shot", [(V + "Idiophones/Struck Idiophones/Vibraphone/Soft Mallets/Vibes_soft_%s_v1_rr1_Main.wav" % n, n)
                               for n in ("D4", "A4", "E5")], dict(length=1.8)),
    ("Glock", "Glck", "shot", [(V + "Idiophones/Struck Idiophones/Glockenspiel/glock_medium_%s_01.wav" % n, n) for n in ("G5", "C6")],
     dict(length=1.4)),
    ("Kalimba", "Kalm", "shot", [(V + "Idiophones/Plucked Idiophones/Kalimba, Kenya/Mbira6_Normal_MainSpirit_%s.wav" % f, n)
                                 for f, n in (("F#3_k10_vl3_rr2", "F#3"), ("B3_k3_vl3_rr2", "B3"), ("F#4_k14_vl3_rr2", "F#4"))],
     dict(length=1.5)),
    ("Pipe Organ", "POrg", "sus", [(V + "Aerophones/Edge-blown Aerophones/Pipe Organ/Quiet/NT5_Man3Quiet_%s_rr1.wav" % n, n)
                              for n in ("A2", "A3", "D#4", "A4")], dict(attack=0.35, loop=0.9)),
    ("Digi E.Piano", "DEP", "shot", [(V + "Electrophones/TX81Z/FM Piano/FMPiano_%s_vl2.wav" % n, n) for n in ("E2", "C3", "G#3", "E4", "C5")],
     dict(length=2.0)),
    ("Aah", "Aah", "sus", [(FS + "764221_CRWDSing_Ahh_Vowel_Sung_Near_F_02_ShaneVincent_G.wav", "F3"),
                           (FS + "211494_choir_aah_wav.wav", "G#4")], dict(attack=0.5, loop=1.0)),
    ("Ooh", "Ooh", "sus", [(FS + "413713_Voice_Choir_OOOH_wav.wav", "C3")], dict(attack=0.5, loop=1.0)),
    ("Sung Chord", "SChd", "sus", [(FS + "274121_Choir_of_Voices_Single_Chord.wav", "E4")], dict(attack=0.5, loop=1.0, chord=True)),
    ("Dream Chord", "Drm", "sus", [(FS + "558416_Mystery_dream_vocal_chord.wav", "E3")], dict(attack=0.5, loop=1.2, chord=True)),
    ("Breath Loop", "BrLp", "tex", [(FS + "554906_Male_Breath_Scared_Frozen_Loop_Stereo_wav.wav", "C4")], dict(start=0.3, loop=2.0)),
    # world instruments. A zone may name a slice of a longer file: (file, root, start s, end s)
    ("Shakuhachi", "Shak", "sus", [(FS + "159550_Skakuhachi_12_Samples_wav.wav", n, a, b)
                                   for n, a, b in (("G#3", 0.03, 6.9), ("F4", 17.84, 25.1), ("C#5", 33.51, 37.7), ("A5", 58.06, 63.6))],
     dict(attack=0.4, loop=1.0)),
    ("Shaku Breath", "ShBr", "tex", [(FS + "500765_shakuhachi_blow_23.wav", "C4")], dict(start=4.0, loop=2.0)),
    ("Sitar", "Sitr", "shot", [(FS + "754604_Free_Style_Sitar.wav", "D#4", 0.0, 2.68), (FS + "754604_Free_Style_Sitar.wav", "G#4", 11.63, 13.55)],
     dict(length=1.9)),
    ("Tanpura", "Tanp", "shot", [(FS + "416606_Tanpura_note_A_sharp_wav.wav", "A#2"), (FS + "416605_Tanpura_note_C_sharp_wav.wav", "C#3")],
     dict(length=2.8)),
    ("Dan Tranh", "DanT", "shot", [(V + "Chordophones/Zithers/Dan Tranh/Vibrato/%s_vib_mf_1.wav" % n, n) for n in ("B2", "F#3", "C#4", "G#4")],
     dict(length=2.4)),
    ("Duduk", "Dudk", "sus", [(FS + "479213_duduk_C3.wav", "C3")], dict(attack=0.4, loop=1.0)),
    ("Erhu", "Erhu", "sus", [(FS + "569838_4_Erhu_E4_wav.wav", "E4")], dict(attack=0.35, loop=0.9)),
    ("Sarangi", "Sarg", "sus", [(FS + "167088_Sarangi_G3_wav.wav", "G#3")], dict(attack=0.35, loop=0.9)),   # named G3, plays G#3
    ("Koto", "Koto", "shot", [(FS + "223956_Koto_G4_wav.wav", "G4")], dict(length=2.0)),
    ("Singing Bowl", "Bowl", "shot", [(FS + "535950_E_flat_Tibetan_singing_bowl_struck.wav", "D5")], dict(length=4.0)),
    ("Didgeridoo", "Didg", "tex", [(V + "Aerophones/Lip Aerophones/Didgeridoo/Didgeridoo1_Sus2_Main.wav", "C#2")], dict(start=1.5, loop=2.0)),
]
# Every set is an Atmos PCM type; the PCM B knob picks the set Variation crossfades into.


# ---- audio --------------------------------------------------------------------------------------
def read_riff(path):
    """WAV chunks by hand, for what the wave module refuses: IEEE float (format 3) and
    WAVE_FORMAT_EXTENSIBLE (0xFFFE)."""
    import struct
    b = open(path, "rb").read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError("not a WAV: " + path)
    i, fmt, data = 12, None, None
    while i + 8 <= len(b):
        cid, n = b[i:i + 4], struct.unpack_from("<I", b, i + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", b, i + 8)
            if fmt[0] == 0xFFFE:   # extensible: the real format is the sub-format GUID's first two bytes
                fmt = (struct.unpack_from("<H", b, i + 8 + 24)[0],) + fmt[1:]
        elif cid == b"data":
            data = b[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)
    tag, ch, sr, _, _, bits = fmt
    if tag == 3:
        x = np.frombuffer(data[:len(data) // (bits // 8) * (bits // 8)], "<f4" if bits == 32 else "<f8").astype(float)
    elif tag == 1 and bits == 24:
        u = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        x = ((u[:, 0] | (u[:, 1] << 8) | (u[:, 2] << 16)) << 8 >> 8) / 8388608.0
    elif tag == 1 and bits in (16, 32):
        x = np.frombuffer(data[:len(data) // (bits // 8) * (bits // 8)], "<i2" if bits == 16 else "<i4") / float(2 ** (bits - 1))
    else:
        raise ValueError("unsupported WAV format %d / %d bit: %s" % (tag, bits, path))
    return x[:len(x) // ch * ch].reshape(-1, ch).mean(axis=1), sr


def read(path):
    try:
        wave.open(path).close()
    except wave.Error:
        return read_riff(path)
    with wave.open(path) as w:
        n, sr, sw, ch = w.getnframes(), w.getframerate(), w.getsampwidth(), w.getnchannels()
        raw = w.readframes(n)
    if sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        x = ((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8 >> 8) / 8388608.0
    elif sw == 2:
        x = np.frombuffer(raw, "<i2") / 32768.0
    elif sw == 4:
        x = np.frombuffer(raw, "<i4") / 2147483648.0
    else:
        x = (np.frombuffer(raw, np.uint8) - 128) / 128.0
    return x.reshape(-1, ch).mean(axis=1), sr


def resample(x, fs_in, fs_out, taps=48):
    """Windowed-sinc (Kaiser) fractional resampler; the cutoff sits just under the lower Nyquist."""
    ratio = fs_in / fs_out
    n_out = int(len(x) / ratio)
    fc = min(1.0, 1.0 / ratio) * 0.94
    k = np.arange(-taps // 2 + 1, taps // 2 + 1)
    win_beta = 8.0
    xp = np.concatenate([np.zeros(taps), x, np.zeros(taps)])
    out = np.empty(n_out)
    for c0 in range(0, n_out, 8192):
        n = np.arange(c0, min(n_out, c0 + 8192))
        t = n * ratio
        base = np.floor(t).astype(np.int64)
        frac = t - base
        idx = base[:, None] + k[None, :] + taps
        d = frac[:, None] - k[None, :]
        r = np.clip(2.0 * d / (taps + 1), -1.0, 1.0)
        w = fc * np.sinc(fc * d) * np.i0(win_beta * np.sqrt(1.0 - r * r)) / np.i0(win_beta)
        out[n] = (xp[idx] * w).sum(axis=1)
    return out


def onset(x, thr=0.02):
    pk = np.max(np.abs(x)) or 1.0
    i = int(np.argmax(np.abs(x) > thr * pk))
    return max(0, i - int(0.004 * SR))


def corr(a, b):
    a, b = a - a.mean(), b - b.mean()
    d = math.sqrt(float((a * a).sum() * (b * b).sum())) or 1.0
    return float((a * b).sum()) / d


def period_of(x, f0):
    """The period (samples, fractional) of a steady segment near the expected f0, by autocorrelation."""
    p0 = SR / f0
    lo, hi = int(p0 / 2 ** (1.5 / 12)), int(p0 * 2 ** (1.5 / 12)) + 1
    seg = x - x.mean()
    best, bp = -2, p0
    for p in range(max(2, lo), hi + 1):
        c = corr(seg[:-p], seg[p:])
        if c > best:
            best, bp = c, p
    # parabolic refine
    p = int(round(bp))
    cs = [corr(seg[:-q], seg[q:]) for q in (p - 1, p, p + 1)]
    den = cs[0] - 2 * cs[1] + cs[2]
    return p + (0.5 * (cs[0] - cs[2]) / den if den else 0.0), best


def find_loop(x, a0, a1, length, period=None):
    """Best (start, length): start in [a0, a1), length near `length` (in whole periods when pitched),
    both multiples of 4 (so the mips keep the same loop), maximising the match of the two ends."""
    W = 384
    cands = [length]
    if period:
        n = max(1, round(length / period))
        cands = [round(m * period) for m in range(max(1, n - 3), n + 4)]
    cands = sorted(set(int(c) // 4 * 4 for c in cands if c > W * 2))
    best = (-2, a0 // 4 * 4, cands[0])
    for L in cands:
        for s in range(a0 // 4 * 4, a1, 8):
            if s + L + W >= len(x):
                break
            c = corr(x[s - W:s + W // 2], x[s + L - W:s + L + W // 2])
            if c > best[0]:
                best = (c, s, L)
    return best


def crossfade(x, ls, le, n, linear):
    t = (np.arange(n) + 0.5) / n
    a, b = (1 - t, t) if linear else (np.cos(t * np.pi / 2), np.sin(t * np.pi / 2))
    x = x.copy()
    x[le - n:le] = x[le - n:le] * a + x[ls - n:ls] * b
    return x


def mips_of(x, ls, le, looped):
    """mip 0 plus 2 octave-down copies, decimated from the signal continued through its own loop so
    each level wraps cleanly"""
    out = [x]
    for k in (1, 2):
        ext = np.concatenate([x[:le], x[ls:ls + 4096]]) if looped else np.concatenate([x, np.zeros(256)])
        d = resample(ext, SR, SR // 2 ** k)[:len(x) // 2 ** k]
        out.append(d)
    return out


def build_zone(kind, opt, path, root, t0=None, t1=None):
    x, fs = read(os.path.join(SRC, path))
    if t0 is not None:   # a slice of a longer file (one note of a multisample take)
        x = x[int(t0 * fs):int(t1 * fs)]
    x = resample(x, fs, SR) if fs != SR else x
    x = x[onset(x):]
    f0 = 440.0 * 2 ** ((root - 69) / 12.0)
    tune, match = 0.0, None
    if kind == "shot":
        # fine-tune from just after the strike (pitched one-shots; inharmonic ones stay at the named root)
        seg = x[int(0.08 * SR):int(0.33 * SR)]
        if len(seg) > SR // 10:
            per, pc = period_of(seg, f0)
            t = 1200 * math.log2((SR / per) / f0)
            if abs(t) <= 30 and pc >= 0.6:   # struck tones are inharmonic: only trust small corrections
                tune = t
        n = min(len(x), int(opt["length"] * SR)) // 4 * 4
        x = x[:n].copy()
        fl = int(0.25 * SR)
        x[-fl:] *= np.linspace(1, 0, fl) ** 2
        ls = le = n
    else:
        if kind == "tex":
            # a texture has no attack worth keeping: start just ahead of its loop (crossfade + 50 ms)
            pre = int(0.45 * SR)
            x = x[max(0, int(opt["start"] * SR) - pre):]
            a0 = pre
            per = None
        else:
            a0 = int(opt["attack"] * SR)
            mid = x[a0:a0 + int(0.25 * SR)]
            per = None
            if not opt.get("chord"):
                per, pc = period_of(mid, f0)
                tune = 1200 * math.log2((SR / per) / f0)
                if abs(tune) > 45 or pc < 0.5:
                    print("   %s: pitch unsure (%+.0f ct, r=%.2f) - left at the named root" % (os.path.basename(path), tune, pc))
                    tune, per = 0.0, None
        L = int(opt["loop"] * SR)
        a1 = a0 + int(0.25 * SR)
        need = a1 + L + int(0.05 * SR)
        if len(x) < need:   # short file: shrink the loop to fit
            L = int((len(x) - a1 - int(0.05 * SR)) * 0.9)
        match, ls, L = find_loop(x, a0, a1, L, per)
        if per and match < 0.8:
            # the period guess can be an octave out (or the tone has no steady period):
            # also try any length within +-12% and keep the better match
            m2 = max((find_loop(x, a0, a1, Lc) for Lc in range(int(L * 0.88) // 4 * 4, int(L * 1.12), 16)), key=lambda r: r[0])
            if m2[0] > match:
                match, ls, L = m2
        le = ls + L
        xf = int((0.4 if kind == "tex" else 0.12) * SR)
        xf = min(xf, ls - 8, L // 2)
        x = crossfade(x, ls, le, xf, match > 0.85)
        x = x[:le]
    return dict(x=x, ls=ls, le=le, root=root + tune / 100.0, tune=tune, match=match, oneshot=kind == "shot", path=path)


def overview(x, n=48):
    seg = np.array_split(np.abs(x), n)
    v = np.array([s.max() if len(s) else 0 for s in seg])
    return [round(float(a), 3) for a in v / (v.max() or 1)]


def main():
    os.makedirs(OUT, exist_ok=True)
    sources = {e["file"]: e for e in json.load(open(os.path.join(OUT, "SOURCES.json")))}
    blob, sets_out, total_s = [], [], 0.0
    off = 0
    for name, short, kind, zones, opt in SETS:
        zs = []
        print("%s (%s)" % (name, kind))
        for zspec in zones:
            path, note = zspec[0], zspec[1]
            if path not in sources:
                sys.exit("not in SOURCES.json (run fetch_sources.py manifest): " + path)
            z = build_zone(kind, opt, path, midi(note), *zspec[2:])
            print("   %-58s root %-4s %+5.1f ct  %.2fs%s" % (os.path.basename(path)[:58], note, z["tune"], len(z["x"]) / SR,
                  "" if z["oneshot"] else "  loop %.2fs match %.2f" % ((z["le"] - z["ls"]) / SR, z["match"])))
            zs.append(z)
        # level the set: the loops (or the first 0.5 s of a one-shot) to -16 dBFS RMS, no clipping
        ref = [z["x"][z["ls"]:z["le"]] if not z["oneshot"] else z["x"][:SR // 2] for z in zs]
        rms = math.sqrt(float(np.mean(np.concatenate(ref) ** 2))) or 1.0
        g = 10 ** (-16 / 20) / rms
        g = min(g, 0.98 / max(float(np.max(np.abs(z["x"]))) for z in zs))
        zo = []
        for z in sorted(zs, key=lambda q: q["root"]):
            levels = mips_of(z["x"] * g, z["ls"], z["le"], not z["oneshot"])
            offs, lens = [], []
            for k, d in enumerate(levels):
                ls_k, le_k = z["ls"] >> k, z["le"] >> k
                guard = d[ls_k:ls_k + GUARD] if not z["oneshot"] else np.zeros(GUARD)
                q = np.clip(np.round(np.concatenate([d[:le_k], guard]) * 32767), -32767, 32767).astype("<i2")
                offs.append(off)
                lens.append(le_k)
                blob.append(q.tobytes())
                off += len(q)
            total_s += len(z["x"]) / SR
            zo.append(dict(file=z["path"], sha256=sources[z["path"]]["sha256"], root=round(z["root"], 3),
                           loop_start=z["ls"], loop_end=z["le"], oneshot=z["oneshot"], offsets=offs, lens=lens))
        sets_out.append(dict(name=name, short=short, kind=kind, chord=bool(opt.get("chord")), zones=zo))
    data = b"".join(blob)
    # UI overviews from mip 0 of the zone nearest E4
    arr = np.frombuffer(data, "<i2")
    for s in sets_out:
        z = min(s["zones"], key=lambda q: abs(q["root"] - 64))
        s["overview"] = overview(arr[z["offsets"][0]:z["offsets"][0] + z["lens"][0]].astype(float))
        s["loop"] = None if z["oneshot"] else [round(z["loop_start"] / z["lens"][0], 3), 1.0]
    types = [dict(name=st["name"], short=st["short"], a=i) for i, st in enumerate(sets_out)]
    with open(os.path.join(OUT, "rom.bin"), "wb") as f:
        f.write(data)
    # the browser build carries mips 1 and 2 only (16 / 8 kHz): ~43% of the size, same zones and loops
    web, woff = [], 0
    for st in sets_out:
        for z in st["zones"]:
            z["web_offsets"] = [-1]
            for k in (1, 2):
                n = z["lens"][k] + GUARD
                web.append(arr[z["offsets"][k]:z["offsets"][k] + n].tobytes())
                z["web_offsets"].append(woff)
                woff += n
    with open(os.path.join(OUT, "rom_web.bin"), "wb") as f:
        f.write(b"".join(web))
    json.dump(dict(sr=SR, mips=MIPS, guard=GUARD, samples=len(arr), sha256=hashlib.sha256(data).hexdigest(),
                   sets=sets_out, types=types), open(os.path.join(OUT, "rom.json"), "w"), indent=1)
    print("ROM: %d sets, %d zones, %.1f s of mip-0 audio, %.2f MB (budget %.0f MB); web ROM %.2f MB" % (
        len(sets_out), sum(len(s["zones"]) for s in sets_out), total_s, len(data) / 1048576, BUDGET / 1048576, woff * 2 / 1048576))
    if len(data) > HARD_LIMIT:
        sys.exit("ROM over %d MB: check the recipes" % (HARD_LIMIT >> 20))
    if len(data) > BUDGET:
        print("note: over the %d MB soft budget" % (BUDGET >> 20))


if __name__ == "__main__":
    main()
