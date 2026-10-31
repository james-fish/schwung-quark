# Third-party notices

## Amino (MIT)

Quark adapts code and structure from two MIT-licensed Schwung modules
(https://github.com/klaine/amino, https://github.com/klaine/crest), Copyright (c) 2026 Amino / Crest contributors:

- `src/dsp/plugin_api.h` - the Schwung plugin ABI header (from Schwung, as vendored by Amino).
- `src/dsp/fx.c` - the 8-line Hadamard FDN reverb structure, delay-line helpers.
- `web/wasm_host.c`, `web/worklet.js` - the WebAssembly / AudioWorklet host pattern.
- `scripts/deploy.sh` - the staged on-device install.
- The contract layout (generated `chain_params` / `ui_hierarchy`, `card_script` cards, `custom:` widgets).

**Amino's 74 wavetables** (64 waves x 128 signed 8-bit samples each) are included as
`tools/data/amino_wt.bin`. Quark builds the first 61 waves of each into `src/dsp/amino_gen.c`; the last
three are the same triangle, square and saw in every table, so they are dropped, with their names in
`tools/data/amino_wt_names.json`. They were extracted from Amino's released `dsp.so` (symbol
`amino_wavetable_data`). Quark band-limits them into mip levels at run time. Where a name clashes with a
Quark type, the Amino table carries a " WT" suffix.

```
MIT License

Copyright (c) 2026 Amino contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## AKWF-FREE (CC0)

Amino's wavetables 32-47 and 68-73 (Cello, Violin, Voice, EPiano, Clavinet, E-Organ, FM, Chip, VGame, Sax, Flute, Overtone, Crushed, Granular, Upright, Piano, E-Bass, E-Guitar, Distorted, Theremin, Raw, Drawn) are built from single-cycle waveforms in AKWF-FREE by Kristoffer Ekstrand
(https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE), released under CC0 1.0. No attribution is
required; it is given with thanks.

## Super Arp (MIT)

The Mono arp follows the design of Super Arp (https://github.com/handcraftedcc/schwung-superarp): the
Leap In / Leap Out orders, the octave-range set, the 40 progression patterns (with chord steps and
fold-back for missing notes), the 40 rhythm patterns and the seeded Drop / velocity / gate / octave /
note variation with a loop length. The pattern and rhythm strings are copied from its `module.json`;
the code in `src/dsp/quark.c` is a new implementation.

```
MIT License

Copyright (c) 2026 Handcrafted Media

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Schwung (MIT)

`src/dsp/plugin_api.h` is from Schwung (https://github.com/charlesvestal/schwung), Copyright (c) 2025-2026
Charles Vestal, MIT.

## Trademarks

No other manufacturer's code, samples, wavetables or presets are used. Quark's own ten wavetables and five
chord stacks are generated from formulas in `tools/gen.py`. Names of other manufacturers' products are not used in Quark's interface,
presets or module description.
