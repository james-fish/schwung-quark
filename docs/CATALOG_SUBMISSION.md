# Submitting Quark to the Schwung module catalog

The catalog is `module-catalog.json` in `charlesvestal/schwung` (catalog v2). It is served live from that
repo's `main`, so an entry takes effect for users as soon as the PR merges. Schwung Manager then reads
`release.json` from this repo's `main` and downloads the tarball it points at.

## Checklist

- [x] `src/module.json` `version` 0.3.0; `release.json` at the repo root points at
      `releases/download/v0.3.0/quark-module.tar.gz`.
- [x] Tarball `quark-module.tar.gz` extracts to `quark/` holding `module.json`, `dsp.so`, `canvas.js`,
      `cards.js`, `help.json`, `LICENSE` and `THIRD_PARTY_NOTICES.md`.
- [x] `dsp.so`: aarch64, glibc <= 2.35 (needs 2.27), no libmvec, exactly one export
      (`move_plugin_init_v2`). Checked by `scripts/so_gate.sh` at the end of every build.
- [x] Schwung's `tools/param-pages/validate.mjs quark` finds no warnings (the fixture had Quark's contract
      injected).
- [x] CI (`.github/workflows/ci.yml`) builds and tests on every push. `release.yml` runs on a `v*` tag:
      it checks that the tag matches `module.json`, builds, tests, attaches the tarball to the release and
      rewrites `release.json` on `main`.
- [x] Licences: MIT `LICENSE`. Amino code and tables (MIT), AKWF-FREE (CC0), Super Arp design (MIT) and
      Schwung's `plugin_api.h` (MIT) are credited in `THIRD_PARTY_NOTICES.md`. No other manufacturer's
      names are used in the UI, presets or description.
- [ ] **Make the repo public.** The catalog fetches `release.json` without auth, so a private repo cannot
      be listed (Omega's PR #585 was bounced for exactly this).
- [ ] Tag `v0.3.0` and push the tag, so the release workflow publishes the tarball.
- [ ] Open the catalog PR (below).

## Catalog entry

Append to the `modules` array of `module-catalog.json`. The array is not sorted, and new entries go at
the end.

```json
{
  "id": "quark",
  "name": "Quark",
  "description": "String and pad machine plus a mono synth: divide-down strings, 89 wavetables (chord stacks included) and FM through 11 filter types, drive, detune bloom and ensemble; a wavetable Mono layer with a chord-following arp; multi-FX and shimmer reverb. 36 presets",
  "author": "James Fish",
  "component_type": "sound_generator",
  "subcategory": "polysynth",
  "tags": ["chord", "wavetable", "fm"],
  "github_repo": "james-fish/schwung-quark",
  "default_branch": "main",
  "asset_name": "quark-module.tar.gz",
  "min_host_version": "1.7.3"
}
```

- **Subcategory and tags:** `polysynth` implies `polyphonic`. The tags come from the catalog's
  `taxonomy.tags` list.
- **`min_host_version`:** use the host version Quark was tested on. Quark uses the empty-knob holes (`null`
  in `knobs`), the root `subtitle` and a preset browser on root. An older host closes the hole up and shows
  "Main" without the subtitle, so nothing breaks.

## PR text (draft)

> **Catalog: add Quark**
>
> Adds **Quark** to the module catalog: https://github.com/james-fish/schwung-quark (release `v0.3.0`,
> `quark-module.tar.gz`; `release.json` on `main` points at it).
>
> A string and pad machine (`sound_generator`) with two layers:
> - **Pads:** divide-down strings, 89 wavetables including five chord stacks and Amino's 74 tables,
>   and FM, through 11 filter types, with drive, detune bloom and ensemble.
> - **Mono:** a wavetable or VA layer with a chord-following arp.
>
> Then a multi-FX and a shimmer reverb, and 36 presets.
>
> One entry in `module-catalog.json`: `polysynth`, tags `chord`, `wavetable`, `fm`, `min_host_version`
> 1.7.3. Ships `help.json`, an MIT `LICENSE` and `THIRD_PARTY_NOTICES.md`.
>
> Checked:
> - The module's own tests: native DSP, every preset, type and filter; UI drawers; wasm.
> - `validate.mjs quark`: clean.
> - The host catalog tests on this branch.
> - Played on a Move.
>
> **AI tools:** written with Claude Code (Claude Opus 5.5) under my direction.
