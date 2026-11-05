# Quark — User Sample Sets: Plan (later release, not 0.5)

> Status: planned, not started. Do this only after the PCM engine (`pcm-engine`) is merged and released
> and the factory bank is signed off by ear. Nothing here changes the current release.

## 0. Goal

Let users drop their own WAV files into a folder and play them as Atmos Layer sets (Layer A / Layer B),
exactly like the factory sets: one sample pitched across the keyboard, or a multisample with one file per
note. Same playback, Era, Start, Mode, Blend and Tune as the factory sets.

Non-goals: on-device loop editing, slicing, time-stretch, recording, stereo playback, Melody-layer PCM.

## 1. What to copy

- `src/dsp/userwt.c` is the pattern: at create, list the folder and name the slots (names only, so
  `chain_params` can show them at once); a `SCHED_OTHER` worker thread reads and converts the files and
  swaps finished buffers in; the audio thread never allocates, locks or touches a file.
- `tools/build_rom.py` holds the preparation steps to port to C: `resample` (windowed sinc), `onset`,
  `period_of` (tuning), `find_loop`, `crossfade`, `mips_of`, and the per-zone levelling.
- `QPcmZone` (`src/dsp/rom_gen.h`) and the zone pick in `tex.c` (`lowest root at or above the key, else
  the top zone`, Era thinning) stay as they are; user zones only need their own sample base pointer.

## 2. Folder and naming

```
/data/UserData/UserLibrary/Samples/Schwung/Quark Samples/
    Glass Pad/                 one set per subfolder; the folder name is the set name
        GlassPad_C3.wav        note name in the file name = the root (C3 = MIDI 48, sharps as C#3)
        GlassPad_G3.wav
    Kalimba Hit/
        Kalimba_shot.wav       one file = pitched across the whole range; _shot = play once, no loop
```

- Root: the last note name in the file name; without one, detect it (`period_of`), else C3.
- Loops: the WAV `smpl` chunk's first loop if present (most editors write it), else the automatic
  finder; `_shot` (or a file shorter than 0.3 s) means one-shot.
- Accept PCM 8/16/24/32-bit and 32-bit float, any rate, mono or stereo (mixed to mono), like userwt.
- Sorted by name; a set name is kept to the 4-character short name rule for the Layer row (first 4
  letters, de-duplicated like the factory short names).

## 3. Engine

- Slots: a fixed number of user sets (8) appended after the factory sets in the `pl_wave` / `pl_b`
  enums, named at create from the folder list (the same renaming path user wavetables use). Empty
  slots show as `User 1..8` and play nothing.
- Per user set: a runtime zone table (`QPcmZone`-shaped) and a sample buffer (int16, 32 kHz, 3 mips +
  guard samples, same layout as the ROM) allocated by the worker. The zone reader takes a base pointer
  instead of always `quark_rom`; factory sets pass `quark_rom`, user sets their buffer.
- Swap: a set becomes playable when its worker sets a ready flag (atomic); a voice holding the old
  buffer keeps it until note-off (or the worker frees only after a grace period), so nothing is freed
  under a sounding voice.
- Limits: 8 s per sample, 16 zones per set, 16 MB for all user sets together (about 112 KB per second
  of audio with mips); over the cap, the set loads its first zones and the rest are skipped.
- Levelling: each zone to the factory -16 dBFS loop RMS, so Layer Level means the same thing.

## 4. CPU and memory

- Playback cost: identical to a factory set (same reader).
- Load cost: on the worker thread only, at create and when files change. Estimated 10-20 ms per
  5-second sample in C (resampling about 8 M multiply-adds, loop search about 4 M); a full set well
  under a second. The audio thread is never blocked; a note before its set is ready plays silence for
  that set.
- Memory: allocated once per set by the worker, freed on destroy.

## 5. UI

- Layer A / Layer B lists: factory sets, then the user sets by folder name.
- Layer row, Start / Mode widget and the zones card work unchanged: the worker also computes the
  48-point overview, zones, roots and loop bracket the UI reads from the ROM metadata today (exposed the
  way `rom.json` data is, through a get_param the UI asks for).
- Patches store a user set by NAME (`pl_wave_name`, `pl_b_name`), like user wavetables; a missing set
  falls back to Off and the card says so.
- Factory presets never use user sets.

## 6. Tests

- Unit: `smpl` loop read, note-name parsing (C3, F#3, Db4), one-shot rule, caps, level.
- Worker: a temp folder (`QUARK_SAMPLE_DIR`, like `QUARK_WT_DIR`) with a single-file set, a 3-file
  multisample and a broken file; the sets appear, play, and the broken one is skipped.
- Realtime: render while the worker loads; no allocation on the audio thread (the existing gate).
- Web bench: no folder access in the browser; offer a file picker (or leave user sets device-only).

## 7. Docs

README section "Your own samples": folder, naming, loop points, limits, and the note that the
automatic loop finder works best on steady sounds (set loop points in an editor for anything else).

## 8. Risks

- Loop quality of arbitrary audio (the factory Aah fooled the finder at a 0.31 match): prefer `smpl`
  loops, and show the match in the card ("loop: auto, rough") so the user knows to set points.
- Memory on the device: keep the 16 MB cap until measured alongside the factory ROM (14.5 MB).
- Startup time with many large sets: names first, audio later (already the userwt approach).
