/* AudioWorklet host for quark.wasm (adapted from Amino/Crest, MIT).
 * Messages: {t:"set",key,val} {t:"get",id,key} -> {t:"got",id,val} {t:"midi",m:[s,d1,d2]} {t:"tempo",bpm,run}
 * Reports {t:"stat", peak, load} about 6 times a second: load = DSP time / audio time. */
function utf8Write(mem, ptr, cap, s) {
  let p = ptr; const end = ptr + cap - 1;
  for (let i = 0; i < s.length && p < end; i++) {
    let c = s.charCodeAt(i);
    if (c < 0x80) mem[p++] = c;
    else if (c < 0x800) { if (p + 1 >= end) break; mem[p++] = 0xc0 | (c >> 6); mem[p++] = 0x80 | (c & 63); }
    else { if (p + 2 >= end) break; mem[p++] = 0xe0 | (c >> 12); mem[p++] = 0x80 | ((c >> 6) & 63); mem[p++] = 0x80 | (c & 63); }
  }
  mem[p] = 0;
}
function utf8Read(mem, ptr, len) {
  let out = "";
  for (let i = ptr, end = ptr + len; i < end;) {
    const b = mem[i++]; let c = b;
    if (b >= 0xe0) c = ((b & 15) << 12) | ((mem[i++] & 63) << 6) | (mem[i++] & 63);
    else if (b >= 0xc0) c = ((b & 31) << 6) | (mem[i++] & 63);
    out += String.fromCharCode(c);
  }
  return out;
}
const clock = (typeof performance !== "undefined" && performance.now) ? () => performance.now() : () => Date.now();

class QuarkProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.ok = false; this.peak = 0; this.blocks = 0; this.busy = 0;
    try {
      const module = new WebAssembly.Module(options.processorOptions.wasm);
      this.x = new WebAssembly.Instance(module, {}).exports;
      if (this.x._initialize) this.x._initialize();
      this.ok = this.x.wa_init(sampleRate) === 1;
    } catch (err) { this.error = String(err && err.message || err); }
    this.port.onmessage = (e) => this.onMessage(e.data);
    this.port.postMessage({ t: "ready", ok: this.ok, error: this.error, sampleRate });
  }
  mem() { return new Uint8Array(this.x.memory.buffer); }
  onMessage(d) {
    if (!this.ok) { if (d.t === "get") this.port.postMessage({ t: "got", id: d.id, val: null }); return; }
    const x = this.x;
    switch (d.t) {
      case "set":
        utf8Write(this.mem(), x.wa_key(), x.wa_key_cap(), d.key);
        utf8Write(this.mem(), x.wa_val(), x.wa_val_cap(), String(d.val));
        x.wa_set(); break;
      case "get": {
        utf8Write(this.mem(), x.wa_key(), x.wa_key_cap(), d.key);
        const n = x.wa_get();
        this.port.postMessage({ t: "got", id: d.id, val: n < 0 ? null : utf8Read(this.mem(), x.wa_out(), n) });
        break;
      }
      case "midi": x.wa_midi(d.m[0], d.m[1] | 0, d.m[2] | 0); break;
      case "tempo": x.wa_set_tempo(d.bpm, d.run ? 1 : 0); break;
    }
  }
  process(_inputs, outputs) {
    const out = outputs[0];
    if (!this.ok || !out || out.length < 2) return true;
    const l = out[0], r = out[1], n = l.length;
    const t0 = clock();
    this.x.wa_render(n);
    this.busy += clock() - t0;
    const pcm = new Int16Array(this.x.memory.buffer, this.x.wa_pcm(), n * 2);
    let pk = this.peak;
    for (let i = 0; i < n; i++) {
      const a = pcm[2 * i] / 32768, b = pcm[2 * i + 1] / 32768;
      l[i] = a; r[i] = b;
      const m = Math.max(a < 0 ? -a : a, b < 0 ? -b : b);
      if (m > pk) pk = m;
    }
    this.peak = pk;
    if (++this.blocks >= 64) {
      const audioMs = this.blocks * n / sampleRate * 1000;
      this.port.postMessage({ t: "stat", peak: this.peak, load: this.busy / audioMs });
      this.peak = 0; this.blocks = 0; this.busy = 0;
    }
    return true;
  }
}
registerProcessor("quark", QuarkProcessor);
