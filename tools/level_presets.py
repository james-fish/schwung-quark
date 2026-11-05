"""Level the factory bank: measures every preset with tests/level.c and rewrites the LEVEL table in
tools/gen.py so the loudest 0.4 s of an 8-note chord lands in -18.5..-13.5 dB (keys, plucks and arps
-20.5..-13.5), with peaks kept under -1 dB. Volume moves first (it is squared), then the layer volumes.
Run from the repo root: tools/level_presets.py [passes]"""
import importlib, math, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
ZIG = os.environ.get("ZIG", "zig")
BIN = os.path.join(ROOT, "build", "level")
HI, LO, LO_SHORT, PEAK = -13.5, -18.5, -20.5, -1.0
SHORT = ("Keys", "Bells & Plucks", "Arps")


def build():
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen.py")], check=True, cwd=ROOT, capture_output=True)
    dsp = sorted(os.path.join("src", "dsp", f) for f in os.listdir(os.path.join(ROOT, "src", "dsp")) if f.endswith(".c"))
    subprocess.run([ZIG, "cc", "-std=c11", "-O2", "-ffast-math", "-fno-finite-math-only", "-Wno-c23-extensions",
                    "-I", "src/dsp", "-o", BIN] + dsp + ["tests/level.c", "-lm", "-lpthread"], check=True, cwd=ROOT)


def measure(name):
    out = subprocess.run([BIN, name], capture_output=True, text=True, check=True).stdout.split()
    return float(out[0]), float(out[1])


def main():
    passes = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    for it in range(passes):
        build()
        gen = importlib.reload(gen) if "gen" in sys.modules else importlib.import_module("gen")
        level, moved = dict(gen.LEVEL), 0
        short = {n for c, ns in gen.BANK if c in SHORT for n in ns}
        for name, d in gen.PRESETS:
            if name == "Init Strings":
                continue
            loud, peak = measure(name)
            lo = LO_SHORT if name in short else LO
            want = lo - loud if loud < lo else HI - loud if loud > HI else 0.0
            want = min(want, PEAK - peak)
            if abs(want) < 0.5:
                continue
            vol, tv, lv = d.get("vol", 0.75), d.get("tx_vol", 0.8), d.get("ln_vol", 0.0)
            nv = min(1.0, vol * 10 ** (want / 40.0))
            rest = want - 40.0 * math.log10(nv / vol)
            t = {"vol": round(nv, 3)}
            if rest > 0.25:
                g = 10 ** (rest / 20.0)
                if tv > 0: t["tx_vol"] = round(min(1.0, tv * g), 3)
                if lv > 0: t["ln_vol"] = round(min(1.0, lv * g), 3)
            level[name] = {**level.get(name, {}), **t}
            moved += 1
            print("pass %d  %-22s %6.1f dB peak %5.1f -> %+5.1f dB  %s" % (it + 1, name, loud, peak, want, t))
        write(level)
        if not moved:
            break
    build()


def write(level):
    p = os.path.join(ROOT, "tools", "gen.py")
    s = open(p, encoding="utf-8").read()
    body = "".join("\n    \"%s\": dict(%s)," % (n, ", ".join("%s=%r" % kv for kv in level[n].items()))
                   for n in sorted(level))
    s = re.sub(r"LEVEL = \{.*?\n?\}\n", "LEVEL = {%s\n}\n" % body, s, count=1, flags=re.S)
    open(p, "w", encoding="utf-8").write(s)


if __name__ == "__main__":
    main()
