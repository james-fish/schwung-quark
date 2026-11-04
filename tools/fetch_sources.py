#!/usr/bin/env python3
"""Fetch the CC0 recordings the PCM ROM is built from (docs/QUARK_PCM_PLAN.md, section 2.3a).

Raw audio lands in tools/data/rom_src/ (gitignored) and never goes into git. What is committed is
tools/data/rom/SOURCES.json: provider, URL, author, licence as reported at fetch time, sha256.

  python3 tools/fetch_sources.py libs                 sparse-checkout the VCSL / VSCO-2-CE folders
  python3 tools/fetch_sources.py search               Freesound CC0 voice candidates -> candidates.json
  python3 tools/fetch_sources.py auth                 open the Freesound OAuth2 page in the browser
  python3 tools/fetch_sources.py auth-code CODE       trade the code for a token (stored, gitignored)
  python3 tools/fetch_sources.py freesound ID [ID..]  download Freesound ORIGINALS (needs the token)
  python3 tools/fetch_sources.py manifest             write SOURCES.json; fails on any non-CC0 file
  python3 tools/fetch_sources.py shortlist            print the listen-through table

Credentials come from .env (or .env.txt) at the repo root: FREESOUND_API_KEY, FREESOUND_CLIENT_ID,
FREESOUND_CLIENT_SECRET. They are never printed, logged or written anywhere but the request.
Standard library only.
"""
import hashlib
import json
import os
import re
import ssl
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import wave
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "data", "rom_src")
ROM = os.path.join(HERE, "data", "rom")
TOKEN = os.path.join(HERE, "data", ".freesound_token.json")
FS_API = "https://freesound.org/apiv2"


def _tls():
    """Verified TLS everywhere: python.org's macOS Python ships without CA certificates, so fall back
    to certifi or the system bundle (never to an unverified context)."""
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass
    for p in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"):
        if os.path.exists(p):
            return ssl.create_default_context(cafile=p)
    return ssl.create_default_context()


TLS = _tls()

CC0_PATTERNS = [r"creative\s*commons\s*0", r"\bcc0\b", r"publicdomain/zero", r"creative commons zero",
                r"cc0 1\.0 universal"]
# CC-BY material is allowed only when listed here (and credited in THIRD_PARTY_NOTICES.md)
ALLOW_CC_BY = set()

# (library, folder, Quark sound) from the plan's source table
LIB_FOLDERS = [
    ("VCSL", "Idiophones/Friction Idiophones/Wine Glasses/Sustains", "Glass pad / glass attack"),
    ("VCSL", "Idiophones/Struck Idiophones/Vibraphone/Bowed", "Glass pad (bowed vibes)"),
    ("VSCO-2-CE", "Strings/Violin Section/susVib", "Digital strings"),
    ("VSCO-2-CE", "Strings/Viola Section/susvib", "Digital strings"),
    ("VSCO-2-CE", "Strings/Cello Section/susvib", "Digital strings"),
    ("VCSL", "Chordophones/Zithers/Psaltery, Bowed and Plucked/LongBow", "Eerie bowed"),
    ("VSCO-2-CE", "Woodwinds/Flute/susNV", "Breath / flute pad"),
    ("VCSL", "Aerophones/Edge-blown Aerophones/Baroque Alto Recorder/Sustain", "Breath / flute pad"),
    ("VCSL", "Aerophones/Edge-blown Aerophones/Ocarina, Typical/Sustains/Sus", "Breath / flute pad"),
    ("VSCO-2-CE", "Keys/Organ/Rode_Blower.wav", "Air / noise wash"),
    ("VCSL", "Membranophones/Other Membranophones/Ocean Drum", "Air / noise wash"),
    ("VSCO-2-CE", "Percussion/susCymb1-bow-1.wav", "Shimmer / bowed metal"),
    ("VSCO-2-CE", "Percussion/susCymb1-bow-2.wav", "Shimmer / bowed metal"),
    ("VSCO-2-CE", "Percussion/gongscrape_pp.wav", "Shimmer / bowed metal"),
    ("VCSL", "Idiophones/Struck Idiophones/Hand Chimes", "Bell attack"),
    ("VCSL", "Idiophones/Struck Idiophones/Vibraphone/Soft Mallets", "Bell attack"),
    ("VCSL", "Idiophones/Struck Idiophones/Tubular Bells 1", "Bell attack"),
    ("VCSL", "Idiophones/Struck Idiophones/Glockenspiel", "Bell attack"),
    ("VCSL", "Idiophones/Plucked Idiophones/Kalimba, Kenya", "Bell attack"),
    ("VCSL", "Aerophones/Edge-blown Aerophones/Pipe Organ/Quiet", "Organ / hollow"),
    ("VCSL", "Aerophones/Edge-blown Aerophones/Renaissance Organ/8'", "Organ / hollow"),
    ("VSCO-2-CE", "Keys/Organ/Quiet", "Organ / hollow"),
    ("VCSL", "Electrophones/TX81Z/FM Piano", "90s FM keys"),
    ("VCSL", "Electrophones/TX81Z/Clavisynth", "90s FM keys"),
    ("VCSL", "Chordophones/Zithers/Dan Tranh/Vibrato", "World: Dan Tranh zither"),
    ("VCSL", "Aerophones/Lip Aerophones/Didgeridoo", "World: didgeridoo drone"),
    ("VCSL", "Idiophones/Struck Idiophones/Hand Bells, Nepalese", "World: Nepalese bells"),
]
LIB_URL = {"VCSL": "https://github.com/sgossner/VCSL", "VSCO-2-CE": "https://github.com/sgossner/VSCO-2-CE"}

# Freesound: the plan's two known candidates plus voice searches (CC0 only)
FS_KNOWN = [274121, 679486]
# world instruments (CC0, original WAVs; D-50 factory-sound recordings excluded on purpose)
FS_WORLD = [159550, 499726, 500765, 500723, 785871, 416606, 416605, 148850, 167088, 569838, 479213, 223956, 535950,
            339970, 754604]
FS_QUERIES = ["sustained aah", "ooh single note", "choir sustain", "vocal pad", "breath loop",
              "female aah sustain", "male ooh sustain", "choir aah"]
FS_FIELDS = "id,name,username,license,duration,tags,url,type,samplerate,channels,bitdepth,ac_note_name,ac_note_midi"


def creds():
    out = {}
    for name in (".env", ".env.txt"):
        p = os.path.join(ROOT, name)
        if os.path.exists(p):
            for line in open(p):
                m = re.match(r"\s*([A-Z0-9_]+)\s*[=:]\s*(.+?)\s*$", line)
                if m:
                    out[m.group(1)] = m.group(2).strip().strip('"').strip("'")
            break
    return out


def is_cc0(text):
    t = (text or "").lower()
    return any(re.search(p, t) for p in CC0_PATTERNS)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(args, cwd):
    subprocess.run(["git"] + args, cwd=cwd, check=True)


# ---- VCSL / VSCO-2-CE -------------------------------------------------------------------------
def cmd_libs():
    os.makedirs(SRC, exist_ok=True)
    for lib, url in LIB_URL.items():
        d = os.path.join(SRC, lib)
        if not os.path.isdir(d):
            git(["clone", "-q", "--filter=blob:none", "--no-checkout", "--depth", "1", url + ".git", lib], SRC)
        paths = [f for l, f, _ in LIB_FOLDERS if l == lib] + ["LICENSE"]
        git(["sparse-checkout", "init", "--no-cone"], d)
        # no-cone patterns: a folder gets a trailing /, a file stays as is
        pats = ["/" + (p if p.lower().endswith(".wav") or p == "LICENSE" else p.rstrip("/") + "/") for p in paths]
        with open(os.path.join(d, ".git", "info", "sparse-checkout"), "w") as f:
            f.write("\n".join(pats) + "\n")
        git(["checkout", "-q", "HEAD"], d)
        lic = open(os.path.join(d, "LICENSE"), errors="replace").read()
        print("%-10s LICENSE %s" % (lib, "CC0" if is_cc0(lic) else "NOT CC0 - check!"))


# ---- Freesound --------------------------------------------------------------------------------
def urlopen_retry(req, timeout):
    """Freesound rate-limits bursts (HTTP 429): back off and retry rather than fail the batch."""
    for i in range(6):
        try:
            return urllib.request.urlopen(req, timeout=timeout, context=TLS)
        except urllib.error.HTTPError as e:
            if e.code != 429 or i == 5:
                raise
            wait = int(e.headers.get("Retry-After") or 0) or 5 * 2 ** i
            print("   rate limited, waiting %ds" % wait)
            time.sleep(wait)


def fs_get(path, params=None, token=None):
    c = creds()
    params = dict(params or {})
    headers = {}
    if token:
        headers["Authorization"] = "Bearer " + token
    else:
        params["token"] = c["FREESOUND_API_KEY"]
    url = FS_API + path + ("?" + urllib.parse.urlencode(params) if params else "")
    req = urllib.request.Request(url, headers=headers)
    with urlopen_retry(req, 60) as r:
        return json.loads(r.read().decode())


def cmd_search():
    seen, out = set(), []
    for sid in FS_KNOWN:
        try:
            s = fs_get("/sounds/%d/" % sid, {"fields": FS_FIELDS})
            s["query"] = "(plan)"
            out.append(s)
            seen.add(s["id"])
        except Exception as e:
            print("sound %d: %s" % (sid, e))
    for q in FS_QUERIES:
        r = fs_get("/search/text/", {"query": q, "filter": 'license:"Creative Commons 0"', "fields": FS_FIELDS,
                                     "page_size": 15, "sort": "rating_desc"})
        for s in r.get("results", []):
            if s["id"] not in seen:
                s["query"] = q
                out.append(s)
                seen.add(s["id"])
    os.makedirs(SRC, exist_ok=True)
    json.dump(out, open(os.path.join(SRC, "freesound_candidates.json"), "w"), indent=1)
    print("%d candidates -> tools/data/rom_src/freesound_candidates.json" % len(out))


def cmd_auth():
    c = creds()
    url = FS_API + "/oauth2/authorize/?" + urllib.parse.urlencode(
        {"client_id": c["FREESOUND_CLIENT_ID"], "response_type": "code", "state": "quark"})
    webbrowser.open(url)   # not printed: it carries the client id
    print("Opened the Freesound authorisation page in your browser. Log in, press Authorize, then run:")
    print("  python3 tools/fetch_sources.py auth-code <the code it shows>")


def cmd_auth_code(code):
    c = creds()
    data = urllib.parse.urlencode({"client_id": c["FREESOUND_CLIENT_ID"], "client_secret": c["FREESOUND_CLIENT_SECRET"],
                                   "grant_type": "authorization_code", "code": code}).encode()
    with urllib.request.urlopen(urllib.request.Request(FS_API + "/oauth2/access_token/", data=data), timeout=60, context=TLS) as r:
        tok = json.loads(r.read().decode())
    with open(TOKEN, "w") as f:
        json.dump(tok, f)
    os.chmod(TOKEN, 0o600)
    print("Token stored (tools/data/.freesound_token.json, gitignored); expires in %ss." % tok.get("expires_in"))


def cmd_freesound(ids):
    tok = json.load(open(TOKEN))["access_token"]
    d = os.path.join(SRC, "freesound")
    os.makedirs(d, exist_ok=True)
    meta = {}
    mp = os.path.join(d, "meta.json")
    if os.path.exists(mp):
        meta = json.load(open(mp))
    for sid in ids:
        if any(k.startswith("%s_" % sid) for k in meta):
            continue   # already fetched
        s = fs_get("/sounds/%s/" % sid, {"fields": FS_FIELDS})
        if not is_cc0(s["license"]) and str(sid) not in ALLOW_CC_BY:
            print("%s: licence %s is not CC0 - skipped" % (sid, s["license"]))
            continue
        name = "%s_%s.%s" % (sid, re.sub(r"[^A-Za-z0-9]+", "_", s["name"]).strip("_")[:48], s["type"])
        req = urllib.request.Request(FS_API + "/sounds/%s/download/" % sid, headers={"Authorization": "Bearer " + tok})
        with urlopen_retry(req, 300) as r, open(os.path.join(d, name), "wb") as f:
            f.write(r.read())
        meta[name] = {k: s.get(k) for k in ("id", "name", "username", "license", "url", "duration")}
        json.dump(meta, open(mp, "w"), indent=1)   # after each file, so a failure keeps what came down
        print("%s -> rom_src/freesound/%s" % (sid, name))


# ---- manifest + shortlist ---------------------------------------------------------------------
NOTE_RE = re.compile(r"(?<![A-Za-z])([A-G])([#b]?)(-?\d)(?![0-9])")


def note_of(fn):
    m = NOTE_RE.findall(os.path.basename(fn).replace("sharp", "#"))
    if not m:
        return None
    n, acc, octv = m[-1]
    return n + acc + octv


def wav_info(p):
    try:
        with wave.open(p) as w:
            return w.getnframes() / float(w.getframerate()), w.getframerate(), w.getsampwidth() * 8, w.getnchannels()
    except Exception:
        return None


def lib_files():
    for lib, folder, use in LIB_FOLDERS:
        base = os.path.join(SRC, lib, folder)
        files = [base] if os.path.isfile(base) else sorted(
            os.path.join(r, f) for r, _, fs in os.walk(base) for f in fs if f.lower().endswith((".wav", ".flac", ".aif", ".aiff")))
        for p in files:
            yield lib, folder, use, p


def cmd_manifest():
    entries, bad = [], []
    for lib in LIB_URL:
        lic = open(os.path.join(SRC, lib, "LICENSE"), errors="replace").read()
        if not is_cc0(lic):
            bad.append(lib + "/LICENSE")
    for lib, folder, use, p in lib_files():
        lic = open(os.path.join(SRC, lib, "LICENSE"), errors="replace").read().strip().splitlines()[0]
        entries.append({"provider": lib, "url": "%s/blob/HEAD/%s" % (LIB_URL[lib], urllib.parse.quote(os.path.relpath(p, os.path.join(SRC, lib)))),
                        "author": "Versilian Studios (sgossner)", "license": lic, "use": use,
                        "file": os.path.relpath(p, SRC), "sha256": sha256(p)})
    mp = os.path.join(SRC, "freesound", "meta.json")
    if os.path.exists(mp):
        for name, m in json.load(open(mp)).items():
            p = os.path.join(SRC, "freesound", name)
            if not (is_cc0(m["license"]) or str(m["id"]) in ALLOW_CC_BY):
                bad.append(name)
            entries.append({"provider": "Freesound", "url": m["url"], "author": m["username"], "license": m["license"],
                            "use": "Choir / vocal", "file": os.path.relpath(p, SRC), "sha256": sha256(p)})
    os.makedirs(ROM, exist_ok=True)
    json.dump(entries, open(os.path.join(ROM, "SOURCES.json"), "w"), indent=1)
    print("%d files -> tools/data/rom/SOURCES.json" % len(entries))
    if bad:
        sys.exit("NOT CC0 (and not allow-listed): " + ", ".join(bad))


def cmd_shortlist():
    rows = []
    for lib, folder, use, p in lib_files():
        info = wav_info(p)
        rows.append((use, lib, os.path.relpath(p, os.path.join(SRC, lib)), "CC0 1.0", note_of(p) or "-",
                     "%.1fs" % info[0] if info else "?"))
    for use, lib, f, lic, note, dur in rows:
        print("%-26s %-10s %-6s %-5s %7s  %s" % (use, lib, lic, note, dur, f))
    cp = os.path.join(SRC, "freesound_candidates.json")
    if os.path.exists(cp):
        print()
        for s in json.load(open(cp)):
            print("%-26s %-10s %-6s %-5s %6.1fs  %s by %s  %s/%s/%sbit  [%s]  %s" % (
                "Choir / vocal", "Freesound", "CC0" if is_cc0(s["license"]) else "CHECK", s.get("ac_note_name") or "-",
                s["duration"], s["name"], s["username"], s.get("type"), s.get("samplerate"), s.get("bitdepth"),
                s.get("query"), s["url"]))


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    {"libs": lambda: cmd_libs(), "search": lambda: cmd_search(), "auth": lambda: cmd_auth(),
     "auth-code": lambda: cmd_auth_code(a[1]), "freesound": lambda: cmd_freesound(a[1:]),
     "manifest": lambda: cmd_manifest(), "shortlist": lambda: cmd_shortlist()}[a[0]]()
