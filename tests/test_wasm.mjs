// Load build/quark.wasm the way web/worklet.js does, play a chord, check the output.
import fs from "node:fs";
import { fileURLToPath } from "node:url";
const root = fileURLToPath(new URL("..", import.meta.url));
const x = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(root + "build/quark.wasm")), {}).exports;
if (x._initialize) x._initialize();
if (x.wa_init(44100) !== 1) { console.log("wa_init failed"); process.exit(1); }
const mem = () => new Uint8Array(x.memory.buffer);
function put(ptr, s) { const m = mem(); for (let i = 0; i < s.length; i++) m[ptr + i] = s.charCodeAt(i); m[ptr + s.length] = 0; }
function set(k, v) { put(x.wa_key(), k); put(x.wa_val(), String(v)); x.wa_set(); }
function get(k) { put(x.wa_key(), k); const n = x.wa_get(); return n < 0 ? null : Buffer.from(x.memory.buffer, x.wa_out(), n).toString(); }
let fails = 0;
for (const p of [0, 7, 15, 21]) {
  set("preset", p);
  for (const n of [48, 55, 60, 64]) x.wa_midi(0x90, n, 100);
  let peak = 0, nan = 0;
  const t0 = performance.now();
  for (let b = 0; b < 344 * 2; b++) {
    x.wa_render(128);
    const pcm = new Int16Array(x.memory.buffer, x.wa_pcm(), 256);
    for (const s of pcm) { if (s !== s) nan++; peak = Math.max(peak, Math.abs(s / 32768)); }
  }
  const ms = performance.now() - t0;
  for (const n of [48, 55, 60, 64]) x.wa_midi(0x80, n, 0);
  console.log(`preset ${p} ${get("preset_name")}: peak ${peak.toFixed(3)}  wasm load ${(ms / 2000 * 100).toFixed(2)}%`);
  if (!(peak > 0.02) || nan) fails++;
}
const ch = get("chain_params"), uh = get("ui_hierarchy");
try { JSON.parse(ch); JSON.parse(uh); } catch (e) { console.log("contract JSON", e.message); fails++; }
console.log(fails ? "FAILED" : "wasm OK");
process.exit(fails ? 1 : 0);
