# 🎛️ Quark — Lush Atmos, Chord Stacks & Melody Leads for Ableton Move

**Quark** is a two-layer synth module for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. It pairs a big, warm **Atmos** engine with a punchy **Melody** synth — complete with a chord-following arpeggiator, multi-FX and a shimmer reverb.

Whether you're after classic string-machine swells, 90s rompler chord stabs, glassy FM keys, sampled choirs or evolving ambient drones, Quark has you covered — all running natively on the Move at a tiny CPU cost. 🚀

---

## ✨ Highlights

- 🎻 **Divide-down string machine** — phase-locked 16′/8′/4′/2′ registers, just like the classics.
- 🌊 **89 wavetables** — Quark's own 10 tables, 5 brand-new **chord stacks**, and all 74 [Amino](THIRD_PARTY_NOTICES.md) tables.
- 📂 **Your own wavetables** — drop Serum/Vital-style WAV files on your Move and play them on both layers.
- 🎹 **Chord stacks** — a whole voicing in one key: **Fifths**, **Minor 9**, **Major 9**, **Dom 11** and **Fourths**. Turn **Variation** to stack the intervals up — perfect for 90s rompler stabs, ambient pads and DnB chords.
- 🎙️ **Sampled Layer: 32 instruments** — choirs, wine glass, bowed vibes, flutes, organ, sitar, shakuhachi, koto, duduk, erhu and more, under the synth or on their own. Blend two sets (by knob, Motion or the mod wheel), play them Clean, 90s or Lo-Fi, start into the sample, and loop them or let them ring once. All recordings are CC0.
- 🔔 **6 FM voices** — E.Piano, Bells, Keys, Glass, Metal and FM Pad.
- 🎚️ **11 filter types** — LP12/24, HP, BP, Dual BP, Notch, two Combs and a Morph filter.
- 🔥 **Drive, Detune & Bloom** — saturate pre/post filter or across the whole chord, and let chords drift gently out of tune over time.
- 🌀 **Ensemble** — Strings, Chorus, Both and a **Deep** mode for drones.
- 🌀 **Dual-target motion** — point Motion at Variation, Pitch, Filter, Pan or Amp, or any pair of them: the main LFO moves the first while **LFO 2** moves the second at any speed ratio from −4× to +4×. **LFO Mode** gives every voice its own LFOs, scattered (Spread) or restarted by each note (Retrig).
- 🎸 **Melody layer** — saw, pulse, sync or any wavetable, a 4-pole ladder filter, glide, stereo spread and a synced delay. Filter and Wave envelopes decay faster than the amp for snappy plucks, and velocity shapes level, filter and wave.
- 🎼 **Chord-following arp** — Melody can arpeggiate whatever the Atmos are holding, with 10 types, 40 patterns, 40 rhythms and smart randomisation.
- 🌌 **FX** — chorus, flanger, phaser, tremolo, drive, delays and tape echo, plus a shimmer reverb that can freeze.
- 📼 **Vintage** — one knob of analog-style drift for pitch, filter, envelopes and level.
- 💾 **86 factory presets**, grouped by sound (pads, voices, chords, keys, bells, arps, splits, bass and lead, drones) — a quarter of them pair the Atmos with a matching Melody part.

---

## 🧭 Move Pages

Pages are laid out so you always know where you are: **Presets** first, then all **Atmos** pages, then all **Melody** pages, then the shared stuff at the end.

| # | Page | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | **Presets** | browser | | | | | | | |
| 2 | **Atmos** | Type | Variation | Motion | Var Env | Attack | Decay | Sustain | Release |
| 3 | Atmos Filter | Cutoff | Resonance | Filter Type | Filter Env | Mod A | Mod D | Mod S | Mod R |
| 4 | Atmos Drive | Drive | Position | Tone | Spread | Detune | Bloom | Atmos Latch | Keytrack |
| 5 | Atmos Motion | Shape | Rate | Motion | Target | LFO 2 Rate | LFO Mode | Ensemble | Ens Type |
| 6 | Atmos Layer | Layer A | Layer B | Blend | Level | Tune | Start | Mode | Era |
| 7 | **Melody** | Shape | Variation | Tune | | Attack | Decay | Env Mode | Melody Mode |
| 8 | Melody Filter | Cutoff | Resonance | Var Env | Filter Env | Glide | Spread | Delay | Delay Time |
| 9 | Melody Arp | Type | Source | Rate | Octaves | Gate | Swing | Latch | Rhythm |
| 10 | Arp Variation | Pattern | Drop | Velocity | Vel Rand | Gate Rand | Oct Rand | Note Rand | Loop |
| 11 | **Mix & Effect** | Atmos Vol | Melody Vol | Atmos Route | Melody Route | FX Type | FX Ctrl | FX Ctrl 2 | FX Amount |
| 12 | Reverb | Decay | Size | Tone | Amount | Shimmer Pitch | Shimmer Amt | Vintage | Volume |
| 13 | Play | Atmos Transpose | Melody Transpose | Key Mode | Split | Velocity | Mod Wheel | Vibrato | Vib Rate |

💡 **Tip:** Cutoff and Resonance sit on knobs 1–2 on *both* filter pages, so your hands always know where to go.

---

## 📂 Your Own Wavetables

Quark plays your WAV wavetables on both the Atmos and the Melody layer! 🎉

1. Copy `.wav` files to **`/data/UserData/UserLibrary/Samples/Schwung/Wavetables/`** on your Move (for example with `scp`).
2. Reload Quark (pick it again in the slot, or restart Schwung).
3. Your tables appear at the end of **Atmos Type** and **Melody Shape**, named after the files.

What works:

- ✅ Serum- and Vital-style wavetables — 2048 samples per frame, as many frames as you like (up to 64 are used, sampled evenly across the table).
- ✅ Single-cycle waveforms of any length (AKWF and friends).
- ✅ 8/16/24/32-bit PCM or 32-bit float, mono or stereo (the first channel is used).
- ✅ Up to 16 files, in alphabetical order.

**Variation** scans through the frames, just like the built-in tables. Patches remember a table by its file name, so adding or removing files never mixes up your sounds. If a file goes missing, the patch falls back to the default type.

💡 **Tip:** Tables load in the background, so the first note after picking one may take a moment to sound.

---

## 🚀 Getting Started

### Install from the Module Catalog
Open **Schwung Manager**, find **Quark** under Sound Generators, and hit install. That's it! 🎉

### Install manually
1. Download `quark-module.tar.gz` from the [latest release](https://github.com/james-fish/schwung-quark/releases/latest).
2. Upload it in Schwung Manager, **or** run `scripts/deploy.sh` to install over SSH.
3. Rescan modules and start playing.

### Try it in your browser
Download `dist/quark.html` and open it in Chrome, Safari or Firefox — desktop or phone. No install needed. Play with your computer keyboard, the on-screen keys or a MIDI controller.

---

## 🛠️ Building from Source

```sh
python3 -m pip install ziglang   # or: brew install zig
scripts/build.sh                 # generate → test → build dsp.so → package tarball + web page
scripts/build.sh --profile       # build with on-device CPU measurement
scripts/deploy.sh [move.local]   # install to your Move over SSH
```

Everything — parameters, pages, presets and wavetable recipes — lives in `tools/gen.py`. Edit it, rebuild, done.

---

## ⚡ Performance

Quark is designed to be lightweight:

- ~1% of one core on Apple silicon with 8 Atmos voices, a Melody line and full effects (12 Atmos and 8 Melody voices available).
- Control-rate modulation, mip-mapped wavetables and effects that switch themselves off when unused.
- Tested on a real Move. ✅

---

## 🙏 Credits

- **Amino** wavetables (MIT) and **AKWF-FREE** waveforms (CC0)
- Arp design inspired by **[Super Arp](https://github.com/handcraftedcc/schwung-superarp)** (MIT)
- Built for **[Schwung](https://github.com/charlesvestal/schwung)** by Charles Vestal

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for full details.

---

## 📜 License

MIT — see [LICENSE](LICENSE). Have fun making music! 🎶
