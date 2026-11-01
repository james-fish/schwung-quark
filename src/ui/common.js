/* Quark UI helpers shared by canvas.js (cell widgets), cards.js (parameter cards) and the browser
 * test page. tools/gen.py pastes this file (and QUARK_META) into each of them, because Schwung loads
 * every script into its own closure. Every value curve here mirrors src/dsp: change both together. */
var Q = (function () {
  var M = QUARK_META;
  var byKey = {}, byName = {};
  M.params.forEach(function (p, i) { p.index = i; byKey[p.key] = p; byName[p.name] = p; });

  function num(v, d) {
    if (v === null || v === undefined || v === "") return d;
    var x = Number(v);
    return isFinite(x) ? x : d;
  }
  /* values maps may carry host prefixes ("synth:tx_var") or :effective suffixes */
  function val(values, key) {
    var p = byKey[key], d = p ? p["default"] : 0;
    if (!values) return d;
    if (values[key + ":effective"] !== undefined && values[key + ":effective"] !== null) return num(values[key + ":effective"], d);
    if (values[key] !== undefined && values[key] !== null) return num(values[key], d);
    for (var k in values) {
      if (values[k] === null || values[k] === undefined) continue;
      var bare = k.indexOf(":") >= 0 ? k.slice(k.indexOf(":") + 1) : k;
      if (bare === key) return num(values[k], d);
    }
    return d;
  }

  function hz(note) { return 440 * Math.pow(2, (note - 69) / 12); }
  function fmtHz(f) { return f >= 1000 ? (f / 1000).toFixed(f >= 10000 ? 1 : 2) + " kHz" : Math.round(f) + " Hz"; }
  function fmtS(s) { return s < 1 ? Math.round(s * 1000) + " ms" : s.toFixed(s < 10 ? 2 : 1) + " s"; }
  function pct(x) { return Math.round(x * 100) + "%"; }
  function bip(x) { return (x > 0 ? "+" : "") + Math.round(x * 100) + "%"; }
  var NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  function noteName(n) { n = Math.round(n); return NOTE[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 2); }

  function envTime(x) { x = Math.max(0, Math.min(1, x)); return 0.001 + 12 * x * x * x; }
  var REGS = ["Violin", "Viola", "Cello", "Brass", "Organ", "Choir"];

  /* ---- waveforms (one cycle, t in 0..1) ---- */
  function regAt(s, v) {
    var pts = M.strings[s], c = pts.length;
    var x = Math.max(0, Math.min(1, v)) * (c - 1), i = Math.min(Math.floor(x), c - 2), f = x - i;
    return pts[i].map(function (a, j) { return a + (pts[i + 1][j] - a) * f; });
  }
  /* wave-bank previews: 8 frames x 32 int8 samples per table, base64 (tools/gen.py) */
  var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", prev = {};
  function wtPrev(ix) {
    ix = Math.max(0, Math.min(M.wt.length - 1, ix | 0));
    if (prev[ix]) return prev[ix];
    var s = M.wt[ix], out = [], acc = 0, bits = 0;
    for (var i = 0; i < s.length; i++) {
      var c = B64.indexOf(s.charAt(i));
      if (c < 0) continue;
      acc = (acc << 6) | c; bits += 6;
      if (bits >= 8) { bits -= 8; var b = (acc >> bits) & 255; out.push(b > 127 ? b - 256 : b); }
    }
    prev[ix] = out;
    return out;
  }
  /* one sample of wave-bank table ix at position v (0..1) and phase t */
  function bankWave(ix, v, t) {
    var d = wtPrev(ix), F = 8, N = 32, pk = 1;
    var pos = Math.max(0, Math.min(1, v)) * (F - 1), k = Math.min(Math.floor(pos), F - 2), f = pos - k;
    var x = (t - Math.floor(t)) * N, i = Math.floor(x) % N, fr = x - Math.floor(x), j = (i + 1) % N;
    var a = d[k * N + i] + (d[k * N + j] - d[k * N + i]) * fr;
    var b = d[(k + 1) * N + i] + (d[(k + 1) * N + j] - d[(k + 1) * N + i]) * fr;
    for (var q = k * N; q < (k + 2) * N; q++) pk = Math.max(pk, Math.abs(d[q]));
    return (a + (b - a) * f) / pk * 0.95;
  }
  function isAmino(ix) { return ix >= M.wtQuark; }
  /* chord-stack tables name their frames (the voicing Variation is on), else null */
  function chordFrames(ix) { return ix >= M.wtQuarkDense && ix < M.wtQuark ? M.chordFrames[ix - M.wtQuarkDense] : null; }
  /* the drive transfer curve (mirrors q_drive_set / q_drive_run, without the tone filter) */
  function sclip(u) { return u > 1 ? 1 : u < -1 ? -1 : 1.5 * u - 0.5 * u * u * u; }
  function driveCurve(x, amt, pre, ref) {
    pre = pre || 1; ref = ref || 0.8;
    var g = (1 + 24 * amt * amt) * pre, bias = 0.15 * amt, off = sclip(bias);
    var pk = sclip(ref * g + bias) - off, comp = (pk > 1e-4 ? ref / pk : 1) / (1 + 0.6 * amt);
    var wet = Math.min(1, amt * 6), y = (sclip(x * g + bias) - off) * comp;
    return x + (y - x) * wet;
  }
  /* detune offset in cents for chord voice i (mirrors DET_PAT in tex.c) */
  var DET_PAT = [0.62, -0.85, 0.33, -0.48, 1.0, -0.2, 0.78, -0.66, 0.15, -1.0, 0.5, -0.36];
  function detCents(i, det) { return det * det * 45 * DET_PAT[i % 12]; }
  function texWave(type, v, t) {
    var tt = M.texTypes[Math.max(0, Math.min(M.texTypes.length - 1, Math.round(type)))];
    t -= Math.floor(t);
    if (tt.family === 0) {
      var r = regAt(tt.index, v), out = 0, norm = 0;
      var p = [t, (t * 2) % 1, (t * 4) % 1, (t * 8) % 1];
      for (var j = 0; j < 3; j++) { out += r[j] * (2 * p[j] - 1); norm += r[j] * r[j]; }
      for (j = 0; j < 4; j++) { out += r[3 + j] * (p[j] < 0.5 ? 1 : -1); norm += r[3 + j] * r[3 + j]; }
      return norm ? out / Math.sqrt(norm) * 0.7 : 0;
    }
    if (tt.family === 1) return bankWave(tt.index, v, t);
    var fm = M.fm[tt.index], tau = 2 * Math.PI;
    var m1 = Math.sin(tau * t * fm[0]) * v * fm[1], m2 = Math.sin(tau * t * fm[2]) * fm[3] * 0.5;
    return Math.sin(tau * t + m1 + m2);
  }
  function lineShape(shape) { return Math.max(0, Math.min(M.lineVA + M.wt.length - 1, Math.round(Number(shape) || 0))); }
  function lineIsWave(shape) { return lineShape(shape) >= M.lineVA; }
  function lineWave(shape, v, t) {
    t -= Math.floor(t);
    shape = lineShape(shape);
    if (shape >= M.lineVA) return bankWave(shape - M.lineVA, v, t);
    var pwm = Math.min(1, v * 3), w = 0.5 - 0.35 * pwm;
    var saw = 2 * t - 1, pul = t < w ? 1 : -1;
    shape = Math.round(shape);
    if (shape === 0) return saw * 0.9;
    if (shape === 1) return pul * 0.85;
    if (shape === 2) return 0.5 * (saw + pul);
    var s2 = (t * 2.6) % 1;
    return 2 * s2 - 1;
  }
  /* the Animate LFO shapes (mirrors lfo_shape in tex.c); S&H / Random use fixed dice per cycle */
  var DICE = [0.6, -0.8, 0.2, 0.9, -0.4, -0.1, 0.7, -0.6];
  function lfoShape(shape, ph) {
    var c = Math.floor(ph), t = ph - c, h = DICE[((c % 8) + 8) % 8], pv = DICE[((c - 1) % 8 + 8) % 8];
    switch (shape) {
      case 0: return Math.sin(2 * Math.PI * t);
      case 1: return t < 0.5 ? 4 * t - 1 : 3 - 4 * t;
      case 2: return 2 * t - 1;
      case 3: return t < 0.5 ? 1 : -1;
      case 4: return h;
      default: var s = t * t * (3 - 2 * t); return pv + (h - pv) * s;
    }
  }
  function familyName(type) {
    var tt = M.texTypes[Math.max(0, Math.min(M.texTypes.length - 1, Math.round(type)))];
    return ["STRINGS", "WAVETABLE", "FM"][tt.family];
  }
  function variationText(values) {
    var type = Math.round(val(values, "tx_type")), v = val(values, "tx_var");
    var tt = M.texTypes[Math.max(0, Math.min(M.texTypes.length - 1, type))];
    if (tt.family === 0) {
      if (tt.index === 0) {
        var x = v * 5, i = Math.min(4, Math.floor(x)), f = x - i;
        return f < 0.15 ? REGS[i] : f > 0.85 ? REGS[i + 1] : REGS[i] + ">" + REGS[i + 1];
      }
      if (tt.name === "Choir") return ["Ooh", "Oh", "Aah", "Eh", "Eee"][Math.min(4, Math.round(v * 4))];
      return pct(v) + " bright";
    }
    if (tt.family === 1) return isAmino(tt.index) ? "Wave " + (1 + Math.round(v * (M.amWaves - 1))) + "/" + M.amWaves :
      chordFrames(tt.index) ? chordFrames(tt.index)[Math.round(Math.max(0, Math.min(1, v)) * 7)] : "Frame " + (1 + v * 7).toFixed(1) + "/8";
    return "FM index " + (v * M.fm[tt.index][1]).toFixed(2);
  }

  /* reverb pre-delay in seconds, scaled by Reverb Size (mirrors q_reverb) */
  function revPre(size) { return 0.005 + 0.07 * size; }
  /* where Wave Env takes the Mono table position at full envelope: it scales to the room left, so
   * any Variation + Wave Env lands inside the table (mirrors lines.c) */
  function wenvTo(pos, we) { return pos + we * (we >= 0 ? 1 - pos : pos); }

  /* ---- value text in real units (mirrors the DSP) ---- */
  function describe(key, values) {
    var x = val(values, key), p = byKey[key];
    var fxt = Math.round(val(values, "fx_type"));
    switch (key) {
      case "tx_a": case "tx_d": case "tx_r": case "me_a": case "me_d": case "me_r": return fmtS(envTime(x));
      case "ln_a": return fmtS(0.5 * envTime(x));
      case "ln_d": return fmtS(0.005 + 0.9 * envTime(x));
      case "tx_type": return p.opts[Math.round(x)];
      case "an_lfo2": return (x < 0 ? "-" : "+") + Math.abs(x).toFixed(2) + "x";
      case "tx_var": return variationText(values);
      case "flt_cut": return fmtHz(hz(20 + 115 * x));
      case "flt_res": {
        var ftn = byKey.flt_type.opts[Math.round(val(values, "flt_type"))];
        if (ftn === "Morph") return x < 0.02 ? "LP" : x > 0.98 ? "HP" : Math.abs(x - 0.5) < 0.02 ? "BP" : (x < 0.5 ? "LP>BP " : "BP>HP ") + pct(x < 0.5 ? 2 * x : 2 * x - 1);
        if (ftn === "Comb+" || ftn === "Comb-") return "Fdbk " + pct(0.3 + 0.66 * x);
        return pct(x);
      }
      case "ln_cut": return fmtHz(hz(20 + 115 * x));
      case "flt_env": case "ln_fenv": return (x >= 0 ? "+" : "") + Math.round(x * 72) + " st";
      case "an_rate": return (0.03 * Math.pow(333, x)).toFixed(2) + " Hz";
      case "vib_rate": return (3 + 6 * x).toFixed(1) + " Hz";
      case "tx_spread": return "±" + (x * 9).toFixed(1) + " ct";
      case "ln_det": return (x * 50).toFixed(1) + " ct";
      case "ln_glide": return fmtS(1.2 * x * x);
      case "rv_decay": return x > 0.995 ? "Freeze" : fmtS(0.35 * Math.pow(80, x));
      case "rv_tone": return fmtHz(1200 * Math.pow(13, x));
      case "rv_size": return pct(x) + " · pre " + Math.round(1000 * revPre(x)) + " ms";
      case "ln_spread": return Math.abs(x) < 0.02 ? "Centre" : (x < 0 ? "Random " : "Steps ") + pct(Math.abs(x));
      case "ln_dly": return x < 0.003 ? "Off" : pct(x);
      case "sh_pitch": return p.opts[Math.round(x)] + " st";
      case "split": return noteName(x);
      case "arp_swing": return Math.round(50 + 16.5 * x) + "%";
      case "arp_gate": return Math.round((0.05 + 1.95 * x * x) * 100) + "%";
      case "arp_rhy": return M.arpRhythmStr[Math.max(0, Math.min(M.arpRhythmStr.length - 1, Math.round(x)))];
      case "arp_pat": return M.arpPatterns[Math.max(0, Math.min(M.arpPatterns.length - 1, Math.round(x)))];
      case "arp_vel": return Math.round(x) === 0 ? "As played" : String(Math.round(x));
      case "arp_loop": return Math.round(x) === 0 ? "Free" : Math.round(x) + " steps";
      case "tx_depth": case "ln_wenv": return bip(x);
      case "ln_var":
        if (lineIsWave(val(values, "ln_shape"))) {
          var cf = chordFrames(lineShape(val(values, "ln_shape")) - M.lineVA);
          return cf ? cf[Math.round(Math.max(0, Math.min(1, x)) * 7)] : "Pos " + pct(x);
        }
        return pct(x);
      case "tx_drive": return x < 0.003 ? "Off" : "+" + (20 * Math.log10(1 + 24 * x * x)).toFixed(1) + " dB";
      case "tx_dtone": return fmtHz(700 * Math.pow(2, 5 * x));
      case "tx_detune": return "±" + (x * x * 45).toFixed(1) + " ct";
      case "tx_bloom": return x < 0.005 ? "Instant" : fmtS(0.05 + 15 * x * x);
      case "fx_ctrl":
        if (fxt <= 1) return (0.08 * Math.pow(60, x)).toFixed(2) + " Hz";
        if (fxt <= 3) return (0.03 * Math.pow(100, x)).toFixed(2) + " Hz";
        if (fxt <= 5) return (0.03 * Math.pow(150, x)).toFixed(2) + " Hz";
        if (fxt === 6) return (0.5 * Math.pow(28, x)).toFixed(1) + " Hz";
        if (fxt === 7) return "+" + (20 * Math.log10(1 + 30 * x * x)).toFixed(1) + " dB";
        return M.delayDivs[Math.max(0, Math.min(11, Math.floor(x * 11.99)))];
      case "fx_tone":
        if (fxt <= 1) return "Depth " + pct(x);
        if (fxt <= 5) return "Fdbk " + pct(x);
        if (fxt === 6) return "Shape " + pct(x);
        if (fxt === 7) return "Tone " + fmtHz(700 * Math.pow(17, x));
        return "Fdbk " + pct(x * 0.92);
    }
    if (p && p.kind === "e") return p.opts[Math.round(x)];
    if (p && p.kind === "i") return String(Math.round(x)) + (p.unit ? " " + p.unit : "");
    if (p && p.mn < 0) return bip(x);
    return pct(x);
  }
  /* The first steps an arp type plays from a 3-note chord: arrays of pitch indices (0 = lowest note,
   * +3 per octave). Mirrors arp_tick in quark.c (Random shows a fixed sample). */
  function arpPreview(values, steps) {
    var mode = Math.round(val(values, "arp_mode")), oct = M.arpOctSeq[Math.max(0, Math.min(M.arpOctSeq.length - 1, Math.round(val(values, "arp_oct"))))];
    var n = 3, out = [], i, k;
    if (mode <= 0) return out;
    var lo = Math.min.apply(null, oct), hi = Math.max.apply(null, oct);
    if (mode === 7 || mode === 10) {
      var pat = M.arpPatterns[Math.max(0, Math.min(M.arpPatterns.length - 1, Math.round(val(values, "arp_pat"))))];
      var st = mode === 7 ? [[1, 2, 3]] : (pat.match(/\([^)]*\)|\d+/g) || ["1"]).map(function (t) { return (t.match(/\d+/g) || []).map(Number); });
      for (k = 0; k < steps; k++) {
        var o = oct[Math.floor(k / st.length) % oct.length], s0 = st[k % st.length], ch = [];
        s0.forEach(function (ix) {
          var j = ix <= n ? ix : ((ix - 1) % (2 * n - 2)) + 1; if (j > n) j = 2 * n - j;
          if (ch.indexOf(j - 1 + 3 * o) < 0) ch.push(j - 1 + 3 * o);
        });
        out.push(ch);
      }
      return out;
    }
    var ext = [];
    if (mode === 6) oct.forEach(function (o) { [1, 0, 2].forEach(function (j) { ext.push(j + 3 * o); }); });
    else for (var o2 = lo; o2 <= hi; o2++) for (i = 0; i < n; i++) ext.push(i + 3 * o2);
    var T = ext.length, pos = -1, dir = 1, rnd = [0.62, 0.11, 0.85, 0.37, 0.05, 0.71, 0.29, 0.93, 0.48, 0.19, 0.77, 0.55];
    for (k = 0; k < steps; k++) {
      var idx;
      if (mode === 5) idx = Math.floor(rnd[k % 12] * T);
      else if ((mode === 3 || mode === 4) && T > 1) {
        pos += dir; if (pos >= T) { pos = T - 2; dir = -1; } if (pos < 0) { pos = 1; dir = 1; }
        idx = mode === 4 ? T - 1 - pos : pos;
      } else {
        pos = (pos + 1) % T;
        var p2 = pos % T;
        if (mode === 2) idx = T - 1 - pos;
        else if (mode === 8) idx = (p2 & 1) === 0 ? p2 / 2 : T - 1 - (p2 - 1) / 2;
        else if (mode === 9) {
          if (T & 1) { var c = (T - 1) / 2, kk = Math.ceil(p2 / 2); idx = p2 === 0 ? c : c + ((p2 & 1) ? -kk : kk); }
          else { var l0 = T / 2 - 1, h0 = T / 2; idx = p2 === 0 ? l0 : (p2 & 1) ? h0 + (p2 - 1) / 2 : l0 - p2 / 2; }
        } else idx = pos;
      }
      out.push([ext[Math.max(0, Math.min(T - 1, idx))]]);
    }
    return out;
  }

  function ctrlLabel(values) {
    var t = Math.round(val(values, "fx_type"));
    return t === 7 ? "Drive" : t >= 8 ? "Time" : "Rate";
  }

  /* ---- drawing on a Schwung frame ctx ---- */
  function line(ctx, x0, y0, x1, y1) {
    x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
    if (typeof ctx.line === "function") { ctx.line(x0, y0, x1, y1, 1); return; }
    var n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0), 1);
    for (var i = 0; i <= n; i++) ctx.fillRect(Math.round(x0 + (x1 - x0) * i / n), Math.round(y0 + (y1 - y0) * i / n), 1, 1, 1);
  }
  function plot(ctx, fn, x, y, w, h, cycles) {
    var px = null, py = null;
    for (var i = 0; i < w; i++) {
      var s = fn(i / w * (cycles || 1));
      var yy = y + Math.round((0.5 - 0.5 * Math.max(-1, Math.min(1, s))) * (h - 1));
      if (px !== null) line(ctx, px, py, x + i, yy);
      px = x + i; py = yy;
    }
  }
  function tw(ctx, s) {
    if (typeof ctx.textWidth === "function") return ctx.textWidth(s);
    return String(s).length * 6;
  }
  function fit(ctx, s, maxW) {
    s = String(s);
    if (tw(ctx, s) <= maxW) return s;
    while (s.length > 1 && tw(ctx, s + ".") > maxW) s = s.slice(0, -1);
    return s + ".";
  }
  function dotted(ctx, x, y, w) { for (var i = 0; i < w; i += 2) ctx.fillRect(x + i, y, 1, 1, 1); }
  function envShape(ctx, a, d, s, r, x, y, w, h, hi) {
    var la = 0.08 + a, ld = 0.08 + d, ls = 0.35, lr = 0.08 + r, tot = la + ld + ls + lr;
    var X = function (t) { return x + t / tot * (w - 1); }, Y = function (v) { return y + (1 - v) * (h - 1); };
    var pts = [[0, 0], [la, 1], [la + ld, s], [la + ld + ls, s], [tot, 0]];
    for (var i = 0; i < 4; i++) {
      line(ctx, X(pts[i][0]), Y(pts[i][1]), X(pts[i + 1][0]), Y(pts[i + 1][1]));
      if (hi === i) line(ctx, X(pts[i][0]), Y(pts[i][1]) - 1, X(pts[i + 1][0]), Y(pts[i + 1][1]) - 1);
    }
  }
  /* Magnitude of the Pads filter at f Hz (mirrors tex.c: SVF damping per type, comb loop with its
   * damping one-pole and output gain). Types by option name. */
  function filterMag(type, f, fc, res) {
    if (type === "Off") return 1;
    if (type === "Comb+" || type === "Comb-") {
      var fb = (0.3 + 0.66 * res) * (type === "Comb+" ? 1 : -1), w = 2 * Math.PI * f / 48000, th = 2 * Math.PI * f / fc;
      /* loop one-pole a / (1 - (1-a) e^-jw), a = 0.65 */
      var dr = 1 - 0.35 * Math.cos(w), di = 0.35 * Math.sin(w), dm = dr * dr + di * di;
      var lr = 0.65 * dr / dm, li = -0.65 * di / dm;
      var er = Math.cos(th), ei = -Math.sin(th);
      var zr = 1 - fb * (lr * er - li * ei), zi = -fb * (lr * ei + li * er);
      return (1 - 0.6 * Math.abs(fb)) / Math.sqrt(zr * zr + zi * zi);
    }
    var k = type === "LP24" || type === "BP24" ? 2 - 1.6 * res : type === "Dual BP" ? 2 - 1.8 * res : type === "Morph" ? 1 : 2 - 1.94 * res;
    function svf(fc2) {   /* returns [re, im] of the denominator and r */
      var r = f / fc2; return { r: r, dr: 1 - r * r, di: k * r };
    }
    var s = svf(fc), d2 = s.dr * s.dr + s.di * s.di, dm = Math.sqrt(d2);
    var lp = 1 / dm, bp = k * s.r / dm, hp = s.r * s.r / dm;
    if (type === "LP12") return lp;
    if (type === "LP24") return lp * lp;
    if (type === "HP12" || type === "HP") return hp;
    if (type === "BP12" || type === "BP") return 1.6 * bp;
    if (type === "BP24") return 1.6 * bp * bp;
    if (type === "Notch") return Math.abs(s.dr) / dm;
    if (type === "Dual BP") {
      /* complex sum of the two bands: H = j k r / (dr + j k r) */
      var t = svf(Math.min(fc * 2.5, 22000));
      var ar = (k * s.r * s.di) / d2, ai = (k * s.r * s.dr) / d2;
      var e2 = t.dr * t.dr + t.di * t.di, br = (k * t.r * t.di) / e2, bi = (k * t.r * t.dr) / e2;
      return Math.sqrt((ar + br) * (ar + br) + (ai + bi) * (ai + bi));
    }
    if (type === "Morph") {
      /* complex blend: LP = 1/D, BP = j k r / D, HP = -r^2 / D */
      var mo = Math.max(0, Math.min(1, res)), wl = mo < 0.5 ? 1 - 2 * mo : 0, wb = mo < 0.5 ? 2 * mo : 2 - 2 * mo, wh = mo < 0.5 ? 0 : 2 * mo - 1;
      var nr = wl - wh * s.r * s.r, ni = wb * k * s.r;
      return Math.sqrt(nr * nr + ni * ni) / dm;
    }
    return lp;
  }

  function filterCurve(ctx, cutNote, res, type, x, y, w, h) {
    var fc = hz(cutNote), px = null, py = null;
    for (var i = 0; i < w; i++) {
      var f = 20 * Math.pow(1000, i / (w - 1));
      var m = filterMag(type, f, fc, res);
      var db = 20 * Math.log(Math.max(1e-4, m)) / Math.LN10;
      var yy = y + Math.round((1 - Math.max(0, Math.min(1, (db + 30) / 48))) * (h - 1));
      if (px !== null) line(ctx, px, py, x + i, yy);
      px = x + i; py = yy;
    }
  }

  return {
    M: M, byKey: byKey, byName: byName, val: val, num: num, hz: hz, envTime: envTime, texWave: texWave,
    lineWave: lineWave, lineIsWave: lineIsWave, bankWave: bankWave, driveCurve: driveCurve, detCents: detCents, fmtS: fmtS, envTimeS: envTime, familyName: familyName, lfoShape: lfoShape, variationText: variationText, describe: describe,
    ctrlLabel: ctrlLabel, line: line, plot: plot, tw: tw, fit: fit, dotted: dotted, envShape: envShape,
    filterCurve: filterCurve, filterMag: filterMag, noteName: noteName, pct: pct, arpPreview: arpPreview, revPre: revPre, wenvTo: wenvTo
  };
})();
