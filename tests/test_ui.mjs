// Exercise every card and cell widget against a mock Schwung frame ctx, at the frame sizes the host
// uses, for every value of every carded parameter. A throw on the device retires the drawer for the
// session ("one strike"), so this must stay green. Run: node tests/test_ui.mjs
import fs from "node:fs";
import vm from "node:vm";

import { fileURLToPath } from "node:url";
const root = fileURLToPath(new URL("..", import.meta.url));
for (const f of ["build/ui/canvas.js", "build/ui/cards.js"]) vm.runInThisContext(fs.readFileSync(root + f, "utf8"), { filename: f });

const chain = JSON.parse(fs.readFileSync(root + "build/chain_params.json", "utf8"));
let fails = 0, draws = 0, outside = 0;

function mockCtx(w, h) {
  const px = (x, y) => { if (x < 0 || y < 0 || x >= w || y >= h) outside++; };
  return {
    width: w, height: h,
    fillRect(x, y, ww, hh) { if (!isFinite(x + y + ww + hh)) throw new Error("NaN rect"); px(x, y); draws++; },
    print(x, y, s) { if (typeof s !== "string") throw new Error("print non-string " + s); if (!isFinite(x + y)) throw new Error("NaN print"); },
    textWidth(s) { return String(s).length * 5; },
    line(x0, y0, x1, y1) { if (!isFinite(x0 + y0 + x1 + y1)) throw new Error("NaN line"); px(x0, y0); px(x1, y1); draws++; },
    setPixel() {}, drawArc() {}, fillCircle() {}, drawCircle() {},
  };
}

function baseValues() {
  const v = {};
  for (const p of chain) if (p.default !== undefined) v[p.key] = p.default;
  return v;
}

function sweep(p) {
  if (p.type === "enum") return p.options.map((_, i) => i);
  const out = [];
  for (let i = 0; i <= 10; i++) out.push(p.min + (p.max - p.min) * i / 10);
  return out;
}

// cards
for (const p of chain.filter((p) => p.card_script)) {
  for (const x of sweep(p)) {
    for (const [ft, tt, ls] of [[0, 0, 0], [3, 7, 5], [7, 15, 40], [9, 9, 87], [1, 60, 2], [2, 94, 30]]) {
      const values = baseValues();
      values.fx_type = ft; values.tx_type = tt; values.ln_shape = ls;
      values[p.key] = x;
      const ctx = mockCtx(p.card_w, p.card_h);
      try {
        globalThis.quark_card(ctx, { name: p.name, value: String(x), raw: x, values, w: p.card_w, h: p.card_h, nowMs: 1234 });
        globalThis.quark_card(ctx, { name: p.name, value: "--", raw: null, values: {}, w: p.card_w, h: p.card_h });
      } catch (e) { fails++; console.log("card", p.key, x, e.message); }
    }
  }
}

// widgets, at several cell frame sizes
const kinds = globalThis.canvas_overlay.widgetKinds;
const sizes = [[32, 14], [32, 26], [64, 15], [64, 26], [16, 15], [25, 15]];
for (const kind of Object.keys(kinds)) {
  for (const [w, h] of sizes) {
    for (let t = 0; t < 95; t++) {
      for (let v = 0; v <= 1.0001; v += 0.25) {
        const values = baseValues();
        values.tx_type = t; values.tx_var = v; values.ln_shape = t; values.ln_var = v;
        values.tx_drive = v; values.tx_drvpos = t % 3; values.tx_detune = v; values.tx_bloom = 1 - v;
        try { kinds[kind](mockCtx(w, h), { values, group: { keys: [] } }); }
        catch (e) { fails++; console.log("widget", kind, w, h, t, v, e.message); }
      }
    }
  }
}
// prefixed / effective / missing value keys must not break the lookup
try {
  kinds["custom:qwave"](mockCtx(64, 20), { values: { "synth:tx_type": "3", "tx_var:effective": "0.7" } });
  kinds["custom:qwave"](mockCtx(64, 20), { values: {} });
  kinds["custom:qline"](mockCtx(64, 20), {});
} catch (e) { fails++; console.log("widget lookups", e.message); }

console.log(`ui: ${draws} primitives, ${outside} outside frame (clipped by host), ${fails} failures`);
process.exit(fails ? 1 : 0);
