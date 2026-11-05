# Quark — PCM / Partials Engine: Context Plan (draft)

> Paste this into Claude Code at the root of `schwung-quark`. Read the whole thing before touching code.
> Work in phases; stop at the end of each phase, run `scripts/build.sh`, and report CPU + test results.

## 0. Goal

Add a **sample-based ("PCM") oscillator** to Quark's Atmos layer, in the spirit of 90s romplers
(Roland JV-1080/2080, Korg M1/Wavestation/Triton): short PCM multisamples with an attack portion and a
loop, layered as **partials** with Quark's existing engines. The target sound is **90s ambient /
liquid drum & bass pads**: breathy air pads, choir "aahs", glassy bell-attack pads, warm digital
strings,
hollow/vocal sweeps — soft, wide, chorused, drenched in reverb.

Non-goals (v1): a full sampler (no slicing, no time-stretch, no recording), no Melody-layer PCM
(it may come later; design so it's possible), no editing of loop points on the device.

## 1. What already exists (read these first)

- `tools/gen.py` — **single source of truth**: params, pages, type recipes, wavetables, presets.
  Generates `src/dsp/*_gen.{c,h}` and `build/web/meta_gen.js`. Never hand-edit `*_gen.*` files.
- `src/dsp/quark.h` — all state (`QInstance`, `QTexVoice`, `QWave`, user-wavetable structs) and the
  realtime rules: **nothing in `render_block` allocates, locks or does I/O.**
- `src/dsp/tex.c` — Atmos layer. Three families selected by `Q_TEX_FAMILY[type]`:
  `0` divide-down strings, `1` wavetable (mip-mapped, `q_wave_read`), `2` 3-op FM. Per voice:
  control-rate updates every `Q_CTRL` (16) samples, then audio loop into `buf[Q_CTRL]`, then
  filter (SVF / comb / morph), drive, amp env, pan. Detune/Spread uses a second phase (`ph2`).
- `src/dsp/userwt.c` — **the pattern to copy**: scan a folder at create (names only), worker
  `pthread` loads/converts off the audio thread, double buffer (`front`/`back`) swapped atomically,
  audio thread only reads the front buffer. Patches store user tables **by file name**.
- `src/dsp/quark.c` — params, state JSON (`v`, `p`, `tx_user`), MIDI routing, `q_create`
  (note: `module_dir` is currently ignored — we'll need it).
- `scripts/build.sh` — gen → native tests (`tests/test_main.c`) → aarch64 `dsp.so` via zig
  (glibc 2.28 target) → tarball → wasm + `dist/quark.html`. `scripts/so_gate.sh` enforces:
  aarch64, glibc ≤ 2.35, no libmvec, **exactly one export** `move_plugin_init_v2`.
- Instances come from a static pool `g_pool[Q_NUM_SLOTS]` (4). Anything big and read-only (the ROM)
  must be **global and shared**, not per instance.

### Hard rules from the codebase
- Enum options and param keys are **append-only** (presets store indices).
- Short names ≤ 5 chars, short options ≤ 4 chars (Move knob grid).
- `tx_type` = built-ins, then `User 1..16` slots at the end. Adding built-in types shifts the user
  slots' indices — fine, because user tables are restored by name — but **add a test** proving an
  old saved state with a user table still restores correctly after the change.
- Keep the CPU budget: currently ~1% of an Apple-silicon core for 8 voices + Melody + FX. Measure on
  the Move with `scripts/build.sh --profile` (`cpu_us` key) before and after every phase.

## 2. Design

### 2.1 New family: `3 = PCM`
Add a fourth Atmos family. A PCM type is a **multisample set**: up to N zones, each a sample with
`root_key`, `lo_key`/`hi_key`, `loop_start`, `loop_end`, `sr`, optional `vel_lo`/`vel_hi`.

Playback per voice:
- Fractional read position (double or 32.32 fixed — avoid float drift on long loops), increment
  `= (zone_sr / host_sr) * 2^((note - root + tune)/12)`.
- Attack region plays once, then loops `loop_start..loop_end` forever (forward loop for v1;
  ping-pong optional later). Loop seams are **crossfaded offline** in `gen.py` so the runtime loop is
  a plain wrap — no click handling at runtime.
- Interpolation: 4-point Hermite (cheap, sounds "right" for the era). Keep linear as a compile-time
  option for benchmarking.
- Anti-aliasing: pre-build **octave mips** of each sample offline (half-band filter + decimate).
  Pick the mip so the playback ratio stays ≤ ~1.5. Zones every ~minor 3rd–4th keep ratios low.
  A tiny bit of alias is period-correct; the mips stop it getting ugly at the top.
- Variation (`tx_var`) for PCM = **crossfade between two sample sets** in a pair (vector-style, à la
  Wavestation): e.g. "Air → Choir", "Glass → Strings". Animate/LFO already modulates Variation, so
  vector sweeps come for free.
- Everything downstream (filter, drive, amp env, Detune/Bloom partner phase, Spread, Ensemble,
  shimmer reverb) is reused unchanged.

### 2.2 Partials (the JV/M1 trick)
The rompler magic is layering: a PCM **attack/noise partial** + a sustaining tonal partial.
Implement a **PCM Layer** that sits *under* any existing Atmos Type (strings / wavetable / FM / PCM):

**Everything new fits on ONE Move page: 13 "Atmos Layer", 7 knobs + 1 spare.**

| # | key (new) | name | short | range / options (short ≤ 4 chars) |
|---|---|---|---|---|
| 1 | `pl_wave` | PCM Layer | Layer | Off + ROM samples (+ user samples at the end) |
| 2 | `pl_lvl` | Layer Level | Lvl | 0..1 |
| 3 | `pl_pitch` | Layer Pitch | Pitch | −24..+24 st (integer) |
| 4 | `pl_fine` | Layer Fine | Fine | ±50 ct (detune vs. the main partial = classic thickening) |
| 5 | `pl_start` | Layer Start | Start | 0..1 sample start offset (0 = full attack; up = softer) |
| 6 | `pl_mode` | Layer Mode | Mode | `Loop` loop, pre-filter · `Shot` one-shot, pre-filter · `Air` loop, post-filter (bypasses the voice filter so breath stays bright) · `Ping` one-shot, post-filter |
| 7 | `pcm_era` | PCM Era | Era | `Cln` / `90s` / `Lo` (see §2.5) — applies to the PCM family AND the Layer |
| 8 | — | *spare* | | Keep empty for now. Candidates if needed later: Layer Release, Layer Pan Spread |

Deliberately left out to save knobs:
- Layer velocity → uses the existing global **Velocity** (Play page).
- A separate Layer Filter route → folded into `pl_mode` (`Air` / `Ping` are the post-filter modes).
- Layer envelope → the Layer shares the voice amp envelope; `Shot`/`Ping` give the attack-only behaviour.

Update `ui_hierarchy` and `help.json` for page 13. Mixing happens inside `buf[]` before the filter
(`Loop`/`Shot`) or is added after the filter (`Air`/`Ping`). CPU: one extra interpolated read per
sample per voice — measure, and cap the voice count when the Layer is on if needed.

### 2.5 Era (the 90s rompler character)
The 90s sound comes from transposing low-rate samples a long way with cheap interpolation, not from
clean playback. One enum controls all of it, with no extra memory:

| Era | stored rate used | interpolation | anti-alias mips | zones used | bit depth |
|---|---|---|---|---|---|
| `Cln` | full (32 kHz) | 4-point Hermite | yes (ratio ≤ 1.5) | all | 16 |
| `90s` | half-rate mip (~16 kHz equivalent) | linear | **no** (let it alias) | every 2nd zone → more stretch, more formant shift | 16 |
| `Lo` | quarter-rate mip | drop-sample (nearest) | no | every 3rd zone | 12-bit quantise |

- The lower-rate data already exists as the mips, so `90s`/`Lo` just read a lower mip **without**
  re-choosing it to avoid aliasing. Zone skipping is a lookup-table change at note-on.
- Default for the new factory presets: `90s`. Default for the param itself: `Cln`
  (so nothing changes for existing patches, which never had PCM).
- Tune the exact rates and bit depth by ear on the Move; the table above is a starting point.

### 2.3 The "ROM" (content — the real bottleneck)
**Do not use or ship ROM dumps or commercial sample content.** Roland/Korg waveforms are copyrighted;
don't use their patch names in preset names either. Two legal sources:

1. **Generated in `gen.py`** (preferred, reproducible, MIT): render samples offline with numpy-free
   Python (or numpy if added as a build dep — ask first) and bake them into a compact table:
   - `Air` — band-passed noise loop with slow spectral motion (breath pad).
   - `Aah` / `Ooh` — additive vocal with formants + per-harmonic random-walk jitter + breath noise
     (multi-cycle loop, ~0.5–1 s, so it chorus-moves on its own).
   - `Glass Atk` — bright inharmonic bell/mallet transient (FM or modal synthesis) for attack partials.
   - `Digi Str` — multi-cycle, slightly detuned supersaw-ish string loop, darkened.
   - `Hollow` — odd-harmonic, pulse-ish vocal formant loop.
   - `Shimmer` — high sparse partials with slow AM (pairs with the existing shimmer reverb).
   - `Breath Atk` / `Chiff` — short noise transients.
   Target: 32 kHz mono int16, budget **up to ~10 MB** total including mips (decided), embedded via a generated `rom_gen.c`
   (same route as `amino_gen.c`) so it works identically in `dsp.so` and wasm.
2. **CC0 recordings** (preferred for anything organic), processed by `gen.py` the same way. See §2.3a.

### 2.3a CC0 source list (verified licences, 2026-11-04)
Main sources, both CC0 and both on GitHub:
- **VCSL**, Versilian Community Sample Library: `github.com/sgossner/VCSL` (CC0 1.0, LICENSE checked)
- **VSCO-2-CE**, VS Chamber Orchestra Community Edition: `github.com/sgossner/VSCO-2-CE` (CC0 1.0, LICENSE checked)

| Quark sound | Source folder | Use |
|---|---|---|
| Glass pad / glass attack | VCSL `Idiophones/Friction Idiophones/Wine Glasses/Sustains/{Slow,Fast}` (4 notes D#4–D5) | sustain loop; sparse zones = real rompler stretch |
| Digital strings | VSCO `Strings/{Violin,Viola,Cello} Section/susVib` | section loops, darkened + Ensemble |
| Eerie bowed | VCSL `Chordophones/Zithers/Psaltery, Bowed and Plucked/LongBow` | glassy string pad |
| Breath / flute pad | VSCO `Woodwinds/Flute/susNV`; VCSL `Aerophones/Edge-blown Aerophones/{Baroque * Recorder, Ocarina, Typical}` | breathy attack + loop |
| Air / noise wash | VSCO `Keys/Organ/Rode_Blower.wav` (organ blower noise); VCSL `Membranophones/Other Membranophones/Ocean Drum` | `Air` mode noise loops |
| Shimmer / bowed metal | VSCO `Percussion/susCymb1-bow-*.wav`, `Percussion/gongscrape_*.wav` | texture partial under pads |
| Bell attacks | VCSL `Idiophones/Struck Idiophones/{Hand Chimes, Vibraphone, Tubular Bells 1, Glockenspiel}`, `Plucked Idiophones/Kalimba, *` | `Shot`/`Ping` attack partials |
| Organ | VCSL `Aerophones/Edge-blown Aerophones/{Pipe Organ, Renaissance Organ}`; VSCO `Keys/Organ/Quiet` | organ / hollow pads |
| 90s FM keys | VCSL `Electrophones/TX81Z/{FM Piano, Clavisynth}` (user patches sampled from the hardware, shipped as CC0 in VCSL) | sampled FM, very era-correct |

Gap: **choir and vocal "aah/ooh"**. Neither library has voices. Get these from Freesound:
- Use the Freesound APIv2 with `filter=license:"Creative Commons 0"` and queries like `sustained aah`,
  `ooh single note`, `choir sustain`, `breath loop`, `vocal pad` (also tag `multisample`).
- Candidates found so far: "Choir of Voices - Single Chord" by bone666138 (CC0; a sung triad, good
  for a 90s chord-pad sample): `freesound.org/people/bone666138/sounds/274121/`; and the
  f-r-a-g-i-l-e "How Long Choir" pack (CC0; song stems in Bm, so check for usable sustained
  pieces): `freesound.org/people/f-r-a-g-i-l-e/sounds/679486/`.
- **Excluded:** Jovica "Melodrone multisample 13 - Invisible Choir" is CC-BY 4.0, not CC0. Only use
  it if I agree to attribution in `THIRD_PARTY_NOTICES.md`.

**Fetch tooling** (`tools/fetch_sources.py`, run on my machine; raw audio never goes into git):
- VCSL / VSCO: `git clone --filter=blob:none --sparse` and check out only the folders above
  into `tools/data/rom_src/` (gitignored).
- Freesound: needs my API key, plus OAuth2 for original WAV/FLAC files. The API serves previews
  without OAuth2, but those are MP3s, so don't use them for the ROM. Read credentials from
  `.env` (gitignored).
- For every file, write `tools/data/rom/SOURCES.json`: provider, URL, author, licence string as
  returned by the API/LICENSE at fetch time, and a sha256. Fail the build if any licence isn't CC0
  (or an explicitly allow-listed CC-BY with attribution).
- `gen.py` reads only from `rom_src/` + `SOURCES.json`, trims/loops/resamples to 32 kHz mono, and
  emits `rom_gen.c`. Commit `rom_gen.c` + `SOURCES.json`, not the raw files.

**Size (decided: a bigger module is fine).** The ROM budget is up to ~10 MB including mips. As
rough sizing, ~1 s per zone, 6 zones and 12 sets at 32 kHz/16-bit is ~4–5 MB before mips, so
there's room for longer, smoother loops and a few more sets. Still:
- Keep the ROM a single global, read-only copy shared by all 4 instances (never per `QInstance`).
- Put the samples in the read-only data section (`static const int16_t`); don't copy them to the
  heap at create.
- If a giant C array slows `zig cc` too much, switch `rom_gen.c` to `.incbin` via an asm stub, or
  load a `rom.bin` from `module_dir` at create (and embed it separately for wasm).
- wasm: raise `--initial-memory` / `--max-memory` in `build.sh` if needed, and check that
  `dist/quark.html` (wasm embedded as base64, so ~1.33× larger) still loads quickly on a phone.
- Report the final `dsp.so`, tarball and `quark.html` sizes, plus the Move's load time for the
  module.

### 2.4 User samples (cheap win, reuse `userwt.c`)
Scan `/data/UserData/UserLibrary/Samples/Schwung/PCM/` (mirror the wavetable folder). Parse WAV
`fmt `, `data` and the **`smpl` chunk** (root note + loop points); if no `smpl`, treat the whole file
as a loop with root C4 and log it. Load and build mips on the worker thread; cap total memory
(e.g. 32 MB) and per-file length (e.g. 10 s); patches store the sample **by name** (`pl_user`,
`tx_pcm_user` keys in state JSON). Show them at the end of `pl_wave` and the PCM `tx_type` slots.

**Legal guard rails for the user folder:** Quark itself stays a plain "load any WAV" player, like any
sampler. Never ship, bundle, link to or name commercial ROM content. No ROM-extraction tooling,
no "import from Roland/Korg" features, no manufacturer names in presets or docs. In the README,
tell users to load only material they have the rights to use.

### 2.6 Graphical UI (plan this first, then implement — follow the existing paradigm exactly)
Before designing, read `src/ui/canvas.js`, `src/ui/cards.js`, `src/ui/common.js`, `web/app.js`,
`tests/test_ui.mjs`, the `viz=` / `card=` params in `tools/gen.py`, and Schwung's `docs/MODULES.md`
("Custom widgets", `drawCell`, `card_script`). The existing conventions:
- **Knob-cell widgets** (`custom:q*` in `canvas.js`) draw one picture for a *group* of knobs,
  declared in `gen.py` via `viz={"group", "role", "kind"}`. 1-bit drawing (`fillRect(..., 1)`,
  `Q.plot`, `Q.dotted`); a tick on a dotted baseline marks a position; small dots mark the family.
- **Cards** (`cards.js#quark_card`) appear while a knob is held/turned: a header line with the
  name + value in real units (`Q.describe`), a dotted rule, then a small picture. Only knobs whose
  picture adds something the page doesn't already show get a card.
- **Values arrive, never read** — no `getParam` from the UI. All the data a drawer needs must be in
  `meta_gen.js`, generated by `gen.py`.

Proposed (Claude Code: refine this into a short UI spec, show me it, then build):
- `custom:qpcm` — group {Layer, Start, Mode}: the selected sample's **amplitude overview**
  (precomputed peaks, ~64 points per ROM sample, emitted into `meta_gen.js`), a bracket over the
  loop region, a tick for Start, and a small glyph for Mode (loop arrow vs. one-shot; an outline
  marker for the post-filter modes). Off → dotted flat line.
- `custom:qera` — Era: a sine drawn at the Era's resolution (smooth / coarse steps / stair-stepped
  and quantised), so the knob literally shows the grit.
- Cards: **Layer** (sample name, zone count, overview), **Pitch/Fine** (semitones + cents, reuse
  the `qdet` fan idea to show the two partials beating), **Era** (one line, e.g.
  `16k · lin · alias`), **Start** (overview with the start tick large).
- Atmos **Type** widget (`qwave`) for family 3: show the PCM overview instead of a single cycle,
  and **four** family dots; extend `Q.familyName` / `Q.texWave` / `Q.variationText` (Variation for
  PCM = "A ▸ B" set crossfade).
- **User samples:** the UI can't read their data, so draw a generic "USER" overview glyph plus the
  file name. Do not add a getParam path for this.
- Update `web/app.js` (browser UI) and `help.json`; extend `tests/test_ui.mjs` so every new
  drawer renders for every option without throwing and stays inside its cell box.

### 2.7 Sound-design findings that shape the content (research, 2026-11-04)
- Producers in the Good Looking era sampled single synth notes and played chords from them, so
  each note of the chord is stretched by a different amount (Thought-Forms, "Use this 90s sampling
  technique for ethereal pads"). Sparse zones plus Era `90s` reproduce this. Don't add more zones
  "for quality".
- They also recorded high (around C5–C6) and played low, which gives more movement and more crunch.
  In `gen.py`, prefer zone roots at the top of each zone's range, so most notes transpose down.
- Whole-chord samples were common too. A PCM set can be one sampled chord (e.g. the CC0 bone666138
  triad), played chromatically. No engine change needed; flag these sets as "chord" in the UI.
- The D-50's model was a sampled attack plus a synthesized sustain, plus airy texture loops mixed
  into the sustain. That's exactly the Layer + Atmos design, so the plan is on track.
- The M1 "Universe" approach was a filtered choir plus a second oscillator with a pitch-enveloped
  FX sample, under heavy reverb and delay. Factory presets should lean on the shimmer reverb and
  delay. Consider a Layer pitch-envelope amount for the spare knob 8.

## 3. Phases (stop and report after each)

1. **Spike** — one hard-coded generated sample (sine-loop + noise attack) as family 3 in `tex.c`.
   Prove: no clicks at loop seam, no allocation in render, CPU delta on Move for 12 voices.
2. **Sources + ROM pipeline** — first `tools/fetch_sources.py` (§2.3a) and listen-through
   shortlist (send me the candidate list with durations/notes before processing). Then `gen.py`: sample generator → loop crossfade → mips → `rom_gen.c/h` (zones,
   loops, offsets). Add `Q_TEX_FAMILY` entries and new `tx_type` names (append before the User slots).
3. **Engine** — multisample zone lookup at note-on (store zone + mip in `QTexVoice`), Hermite read,
   loop, mip selection at control rate, Variation = set crossfade, Detune/Spread partner phase,
   **Era** (§2.5) read paths. A/B `Cln` vs `90s` on the Move before moving on.
4. **Layer + UI** (done 2026-11-05, revised: PCM types removed from Atmos Type, which gains Off; samples live only in the Atmos Layer page = Layer A, Layer B, Blend, Lvl, Tune, Start, Mode, Era; Blend is a Motion target and wheel destination, and with Type Off Variation modulation moves it; ROM levelled per zone) — params from §2.2 on page 13, pre/post-filter mixing, all families.
   Write the §2.6 UI spec first and get my OK, then the widgets, cards, browser UI and UI tests.
5. **Content + presets** — the §2.3 waves, then ~10 presets aimed at 90s ambient / liquid DnB
   (long attack, LP24 slow sweep, Ensemble Deep, shimmer +12, Vintage ~0.3, Bloom on).
   Original names only.
   Bank audit (done 2026-11-05): 75 -> 88 presets, browsing order by sound (`BANK` in gen.py; states
   save the preset name, 0.4 indices map through `quark_preset_legacy`); every filter, effect, LFO
   mode and Key Mode now used; 23 presets give the wheel a job; levels trimmed by
   `tools/level_presets.py` (spread 21 dB -> 7 dB).
6. **User samples** — §2.4, `smpl` parsing, worker thread, name-based state.
7. **Web + docs** — wasm build (check `--initial-memory`/`--max-memory` headroom), browser UI meta,
   README section "Your Own Samples", `help.json`, version bump, `THIRD_PARTY_NOTICES.md` if CC0 used.

## 4. Tests to add (`tests/test_main.c`, `tests/test_wasm.mjs`)
- Loop seam: render a held note 10 s, assert no sample-to-sample jump above a threshold at wraps.
- Aliasing: sweep a PCM note to C8, measure energy above Nyquist-folded band vs. a reference.
- Zone mapping: every key 0–127 resolves to a zone; ratio ≤ 1.5 after mip choice.
- Realtime: render path never calls malloc (wrap/instrument as `userwt` tests do, if they do).
- State: save/restore with `pl_wave` + user sample by name; missing file falls back to Off.
- Old-state compat: a v0.4.5 state (incl. a user wavetable) restores identically.
- Era: `Cln` alias energy stays under the threshold; `90s`/`Lo` measurably exceed it (proves the
  grit is really there) and never produce NaN/denormals; switching Era while notes play doesn't click.
- CPU benchmark: 12 voices PCM + Layer on, for every Era, compare to current baseline; fail if > agreed budget.
- `so_gate.sh` still passes (still one export, glibc ≤ 2.35).

## 5. Open questions (ask me before deciding)
- (Decided) ROM budget up to ~10 MB. Ask only if the Schwung catalog turns out to have a size limit.
- OK to add numpy as a build-time dependency for sample generation?
- Should Melody get the PCM Layer too (for bell-attack plucks)?
- Voice cap when Layer is on — 12 or drop to 8?
