/* Quark browser test bench.
 *
 * The same C engine that ships as dsp.so runs here as WebAssembly in an AudioWorklet. The left side is
 * a simulated Move: a 128x64 1-bit display laid out like Schwung's knob grid, drawn with Quark's REAL
 * canvas.js widgets and cards.js cards (the files that ship), 8 encoders and a jog. The built-in
 * Schwung widgets (dials, enum boxes, envelope / filter / LFO graphics) are approximations of the
 * host's renderer - close, not pixel-identical. The right side is the panel + detail tabs. Works on
 * phones: the layout stacks, and the on-screen keyboard has a Latch mode (tap to hold, tap to release).
 */
(function () {
  "use strict";
  var M = QUARK_META, P = M.params, byKey = Q.byKey;
  var vals = {};
  P.forEach(function (p) { vals[p.key] = p["default"]; });
  var engine = null, presetIndex = 0, starting = false;
  var listeners = {};          // key -> [fn]
  var touched = null;          // { key, at }
  var effective = {};          // live values from the engine (tx_var:effective)

  /* ------------------------------------------------------------------ engine */
  function b64ToBytes(b64) {
    var bin = atob(b64), out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  /* main-thread fallback (ScriptProcessor) for browsers that refuse a worklet module on file:// */
  function scriptEngine(ctx, an) {
    var x = new WebAssembly.Instance(new WebAssembly.Module(b64ToBytes(QUARK_WASM_B64)), {}).exports;
    if (x._initialize) x._initialize();
    if (x.wa_init(ctx.sampleRate) !== 1) throw new Error("wa_init failed");
    function put(ptr, cap, s) { var m = new Uint8Array(x.memory.buffer), b = new TextEncoder().encode(s), n = Math.min(b.length, cap - 1); m.set(b.subarray(0, n), ptr); m[ptr + n] = 0; }
    var sp = ctx.createScriptProcessor(1024, 0, 2), busy = 0, frames = 0, peak = 0;
    sp.onaudioprocess = function (e) {
      var l = e.outputBuffer.getChannelData(0), r = e.outputBuffer.getChannelData(1), t0 = performance.now();
      for (var o = 0; o < l.length; o += 128) {
        x.wa_render(128);
        var pcm = new Int16Array(x.memory.buffer, x.wa_pcm(), 256);
        for (var i = 0; i < 128; i++) { l[o + i] = pcm[2 * i] / 32768; r[o + i] = pcm[2 * i + 1] / 32768; peak = Math.max(peak, Math.abs(l[o + i]), Math.abs(r[o + i])); }
      }
      busy += performance.now() - t0; frames += l.length;
      if (frames > 8192) { onStat({ peak: peak, load: busy / (frames / ctx.sampleRate * 1000) }); busy = 0; frames = 0; peak = 0; }
    };
    sp.connect(ctx.destination); sp.connect(an);
    return {
      set: function (k, v) { put(x.wa_key(), x.wa_key_cap(), k); put(x.wa_val(), x.wa_val_cap(), String(v)); x.wa_set(); },
      get: function (k) { put(x.wa_key(), x.wa_key_cap(), k); var n = x.wa_get(); return Promise.resolve(n < 0 ? null : new TextDecoder().decode(new Uint8Array(x.memory.buffer, x.wa_out(), n))); },
      midi: function (m) { x.wa_midi(m[0], m[1] | 0, m[2] | 0); },
      tempo: function (bpm, run) { x.wa_set_tempo(bpm, run ? 1 : 0); },
      mode: "ScriptProcessor (main thread)",
    };
  }
  var Engine = {
    start: async function () {
      /* iOS: play through the ringer/silent switch (Safari 17+) */
      try { if (navigator.audioSession) navigator.audioSession.type = "playback"; } catch (e) { /* older Safari */ }
      var Ctx = window.AudioContext || window.webkitAudioContext;
      var ctx = new Ctx({ sampleRate: 44100, latencyHint: "interactive" });
      var an = ctx.createAnalyser(); an.fftSize = 1024;
      var node = null;
      /* a data: URL loads from file:// pages in Chrome; a blob: URL does not (null origin) */
      var urls = ["data:text/javascript;base64," + btoa(QUARK_WORKLET),
                  URL.createObjectURL(new Blob([QUARK_WORKLET], { type: "text/javascript" }))];
      for (var u = 0; u < urls.length && !node; u++) {
        try {
          await ctx.audioWorklet.addModule(urls[u]);
          node = new AudioWorkletNode(ctx, "quark", { numberOfInputs: 0, outputChannelCount: [2],
            processorOptions: { wasm: b64ToBytes(QUARK_WASM_B64).buffer } });
        } catch (e) { node = null; console.warn("worklet route failed:", e.message); }
      }
      if (node) {
        var info = await new Promise(function (res) { node.port.onmessage = function (e) { if (e.data.t === "ready") res(e.data); }; });
        if (!info.ok) throw new Error("engine failed: " + info.error);
        node.connect(ctx.destination); node.connect(an);
        var pend = new Map(), id = 1;
        node.port.onmessage = function (e) {
          var d = e.data;
          if (d.t === "got") { var f = pend.get(d.id); pend.delete(d.id); if (f) f(d.val); }
          else if (d.t === "stat") onStat(d);
        };
        engine = {
          set: function (k, v) { node.port.postMessage({ t: "set", key: k, val: String(v) }); },
          get: function (k) { return new Promise(function (res) { var i = id++; pend.set(i, res); node.port.postMessage({ t: "get", id: i, key: k }); }); },
          midi: function (m) { node.port.postMessage({ t: "midi", m: m }); },
          tempo: function (bpm, run) { node.port.postMessage({ t: "tempo", bpm: bpm, run: run }); },
          mode: "AudioWorklet",
        };
      } else {
        engine = scriptEngine(ctx, an);
      }
      engine.ctx = ctx; engine.an = an;
      await ctx.resume();
      engine.set("state", stateJson());
      engine.tempo(+document.getElementById("bpm").value || 120, running);
      document.getElementById("power").title = "Engine: " + engine.mode;
    }
  };
  function stateJson() {
    var o = {};
    P.forEach(function (p) { o[p.key] = vals[p.key]; });
    return JSON.stringify({ v: 1, preset: presetIndex, p: o });
  }

  /* ------------------------------------------------------------------ params */
  function clampVal(p, v) {
    v = Math.max(p.mn, Math.min(p.mx, v));
    if (p.kind !== "f") v = Math.round(v);
    return v;
  }
  function setParam(key, v, fromUser) {
    var p = byKey[key];
    v = clampVal(p, v);
    if (vals[key] === v) return;
    vals[key] = v;
    if (engine) engine.set(key, v);
    if (fromUser) touched = { key: key, at: performance.now() };
    (listeners[key] || []).forEach(function (f) { f(v); });
    redrawGraphics();
  }
  function on(key, f) { (listeners[key] = listeners[key] || []).push(f); f(vals[key]); }
  function loadPreset(i) {
    presetIndex = (i + M.presets.length) % M.presets.length;
    var pv = M.presets[presetIndex].values;
    P.forEach(function (p, j) { vals[p.key] = pv[j]; });
    if (engine) engine.set("preset", presetIndex);
    P.forEach(function (p) { (listeners[p.key] || []).forEach(function (f) { f(vals[p.key]); }); });
    document.getElementById("preset").value = presetIndex;
    redrawGraphics();
  }

  /* ------------------------------------------------------------------ controls */
  var SVGNS = "http://www.w3.org/2000/svg";
  function knob(key, cls) {
    var p = byKey[key], el = document.createElement("div");
    el.className = "ctl";
    var size = 44, r = 17, cx = 22, cy = 22;
    var svg = document.createElementNS(SVGNS, "svg");
    svg.setAttribute("width", size); svg.setAttribute("height", size);
    svg.setAttribute("class", "knob " + (cls || ""));
    function arcPath(a0, a1) {
      var p0 = [cx + r * Math.sin(a0), cy - r * Math.cos(a0)], p1 = [cx + r * Math.sin(a1), cy - r * Math.cos(a1)];
      var large = Math.abs(a1 - a0) > Math.PI ? 1 : 0, sweep = a1 > a0 ? 1 : 0;
      return "M" + p0[0] + " " + p0[1] + " A" + r + " " + r + " 0 " + large + " " + sweep + " " + p1[0] + " " + p1[1];
    }
    var A0 = -0.75 * Math.PI, A1 = 0.75 * Math.PI;
    var trk = document.createElementNS(SVGNS, "path"); trk.setAttribute("d", arcPath(A0, A1)); trk.setAttribute("class", "trk");
    trk.setAttribute("fill", "none"); trk.setAttribute("stroke-width", "3");
    var arc = document.createElementNS(SVGNS, "path"); arc.setAttribute("class", "arc"); arc.setAttribute("fill", "none"); arc.setAttribute("stroke-width", "3");
    var cap = document.createElementNS(SVGNS, "circle"); cap.setAttribute("cx", cx); cap.setAttribute("cy", cy); cap.setAttribute("r", 13); cap.setAttribute("class", "cap");
    var ptr = document.createElementNS(SVGNS, "line"); ptr.setAttribute("class", "ptr");
    svg.append(trk, arc, cap, ptr);
    var name = document.createElement("div"); name.className = "name"; name.textContent = labelFor(p);
    var val = document.createElement("div"); val.className = "val";
    el.append(svg, name, val);
    var bip = p.mn < 0;
    on(key, function (v) {
      var n = (v - p.mn) / (p.mx - p.mn), a = A0 + (A1 - A0) * n;
      var from = bip ? 0 : A0;
      arc.setAttribute("d", Math.abs(a - from) < 0.01 ? "" : arcPath(Math.min(from, a), Math.max(from, a)));
      ptr.setAttribute("x1", cx + 4 * Math.sin(a)); ptr.setAttribute("y1", cy - 4 * Math.cos(a));
      ptr.setAttribute("x2", cx + 11 * Math.sin(a)); ptr.setAttribute("y2", cy - 11 * Math.cos(a));
      val.textContent = Q.describe(key, vals);
    });
    dragify(svg, key);
    svg.title = p.name;
    return el;
  }
  function labelFor(p) {
    var map = { tx_var: "Variation", tx_anim: "Animate", tx_depth: "Var Env", tx_a: "Attack", tx_d: "Decay", tx_s: "Sustain", tx_r: "Release",
      ln_var: "Variation", ln_a: "Attack", ln_d: "Decay", ens_amt: "Amount", tx_vol: "Volume", ln_vol: "Volume", ln_spread: "Spread", ln_dly: "Delay", ln_dtime: "Time",
      tx_drive: "Drive", tx_drvpos: "Position", tx_detune: "Detune", tx_bloom: "Bloom", tx_latch: "Pads Latch", ln_wenv: "Wave Env",
      arp_mode: "Arp Type", arp_src: "Source", arp_hold: "Arp Latch", arp_oct: "Octaves",
      fx_ctrl: "Control", fx_amt: "Amount", sh_amt: "Amount", rv_decay: "Decay", rv_amt: "Amount", vol: "Volume", vintage: "Vintage",
      tx_type: "Type", ln_shape: "Shape", fx_type: "Type", sh_pitch: "Pitch", ens_type: "Type", ln_sus: "Envelope" };
    return map[p.key] || p.name;
  }
  function dragify(el, key) {
    var p = byKey[key];
    el.addEventListener("pointerdown", function (e) {
      el.setPointerCapture(e.pointerId);
      var y0 = e.clientY, v0 = vals[key];
      function mv(ev) {
        var range = p.mx - p.mn, scale = ev.shiftKey ? 800 : 160;
        var dv = (y0 - ev.clientY) / scale * range;
        setParam(key, p.kind === "f" ? v0 + dv : v0 + Math.round((y0 - ev.clientY) / (range > 20 ? 3 : 14)), true);
      }
      function up() { el.removeEventListener("pointermove", mv); el.removeEventListener("pointerup", up); }
      el.addEventListener("pointermove", mv); el.addEventListener("pointerup", up);
    });
    el.addEventListener("wheel", function (e) {
      e.preventDefault();
      var step = p.kind === "f" ? (p.mx - p.mn) * (e.shiftKey ? 0.002 : 0.012) : 1;
      setParam(key, vals[key] + (e.deltaY < 0 ? step : -step), true);
    }, { passive: false });
    el.addEventListener("dblclick", function () { setParam(key, p["default"], true); });
  }
  function enumCtl(key) {
    var p = byKey[key], el = document.createElement("div");
    el.className = "ctl";
    var name = document.createElement("div"); name.className = "name"; name.textContent = labelFor(p);
    if (p.opts.length <= 5) {
      var seg = document.createElement("div"); seg.className = "seg";
      var btns = p.opts.map(function (o, i) {
        var b = document.createElement("button"); b.textContent = o;
        b.onclick = function () { setParam(key, i, true); };
        seg.appendChild(b); return b;
      });
      on(key, function (v) { btns.forEach(function (b, i) { b.classList.toggle("sel", i === Math.round(v)); }); });
      el.append(seg, name);
    } else if (p.opts.length > 12) {
      /* long lists (95 pad types, 88 mono shapes): a native picker between two steppers */
      var st2 = document.createElement("div"); st2.className = "step";
      var l2 = document.createElement("button"); l2.innerHTML = "&#9664;";
      var sel = document.createElement("select");
      p.opts.forEach(function (o, i) { var op = document.createElement("option"); op.value = i; op.textContent = o; sel.appendChild(op); });
      var r2 = document.createElement("button"); r2.innerHTML = "&#9654;";
      l2.onclick = function () { setParam(key, (Math.round(vals[key]) - 1 + p.opts.length) % p.opts.length, true); };
      r2.onclick = function () { setParam(key, (Math.round(vals[key]) + 1) % p.opts.length, true); };
      sel.onchange = function () { setParam(key, +sel.value, true); };
      st2.append(l2, sel, r2);
      on(key, function (v) { sel.value = Math.round(v); });
      el.append(st2, name);
    } else {
      var st = document.createElement("div"); st.className = "step";
      var l = document.createElement("button"); l.innerHTML = "&#9664;";
      var s = document.createElement("span");
      var r = document.createElement("button"); r.innerHTML = "&#9654;";
      l.onclick = function () { setParam(key, (Math.round(vals[key]) - 1 + p.opts.length) % p.opts.length, true); };
      r.onclick = function () { setParam(key, (Math.round(vals[key]) + 1) % p.opts.length, true); };
      s.onclick = r.onclick;
      dragify(s, key);
      st.append(l, s, r);
      on(key, function (v) { s.textContent = p.opts[Math.round(v)]; });
      el.append(st, name);
    }
    return el;
  }
  function ctl(key, cls) { return byKey[key].kind === "e" ? enumCtl(key) : knob(key, cls); }
  function mount(key, cls) { var host = document.getElementById("c_" + key); if (host) host.appendChild(ctl(key, cls)); }

  /* ------------------------------------------------------------------ panel graphics */
  function cssCtx(canvas) {
    var c = canvas.getContext("2d");
    c.lineWidth = 1.6; c.strokeStyle = "#e9e7e1"; c.fillStyle = "#e9e7e1";
    return c;
  }
  function plotCanvas(canvas, fn, cycles, color) {
    var c = cssCtx(canvas), W = canvas.width, H = canvas.height;
    c.clearRect(0, 0, W, H);
    c.strokeStyle = "#3a3b40"; c.beginPath(); c.moveTo(0, H / 2); c.lineTo(W, H / 2); c.stroke();
    c.strokeStyle = color || "#e9e7e1"; c.beginPath();
    for (var i = 0; i <= W; i++) {
      var y = H / 2 - fn(i / W * cycles) * (H / 2 - 8);
      if (i) c.lineTo(i, y); else c.moveTo(i, y);
    }
    c.stroke();
  }
  function envCanvas(canvas, a, d, s, r, ad) {
    var c = cssCtx(canvas), W = canvas.width, H = canvas.height;
    c.clearRect(0, 0, W, H);
    var la = 0.08 + a, ld = 0.08 + d, ls = ad ? 0 : 0.35, lr = ad ? 0 : 0.08 + r, tot = la + ld + ls + lr;
    var X = function (t) { return 4 + t / tot * (W - 8); }, Y = function (v) { return H - 4 - v * (H - 8); };
    c.beginPath(); c.moveTo(X(0), Y(0)); c.lineTo(X(la), Y(1));
    if (ad) { c.quadraticCurveTo(X(la + ld * 0.15), Y(0.05), X(la + ld), Y(0)); }
    else { c.quadraticCurveTo(X(la + ld * 0.2), Y(s), X(la + ld), Y(s)); c.lineTo(X(la + ld + ls), Y(s)); c.quadraticCurveTo(X(la + ld + ls + lr * 0.2), Y(0), X(tot), Y(0)); }
    c.stroke();
  }
  function filterCanvas(canvas, cut, res, type) {
    var c = cssCtx(canvas), W = canvas.width, H = canvas.height;
    c.clearRect(0, 0, W, H);
    var fc = Q.hz(cut);
    c.beginPath();
    for (var i = 0; i <= W; i++) {
      var f = 20 * Math.pow(1000, i / W);
      var m = Q.filterMag(type, f, fc, res);
      var db = 20 * Math.log10(Math.max(1e-4, m)), y = H - 4 - Math.max(0, Math.min(1, (db + 30) / 48)) * (H - 8);
      if (i) c.lineTo(i, y); else c.moveTo(i, y);
    }
    c.stroke();
  }
  function lfoCanvas(canvas, shape, depth) {
    var rnd = [0.3, -0.8, 0.6, -0.2, 0.9, -0.5, 0.1, -0.9];
    plotCanvas(canvas, function (t) {
      var ph = t % 1, y;
      if (shape === 0) y = Math.sin(2 * Math.PI * ph); else if (shape === 1) y = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
      else if (shape === 2) y = 2 * ph - 1; else if (shape === 3) y = ph < 0.5 ? 1 : -1; else y = rnd[Math.floor(t * 2) % 8];
      return y * Math.max(0.1, depth);
    }, 2, "#b9ab8e");
  }
  var extraGraphics = [];
  function redrawGraphics() {
    var type = vals.tx_type, va = effective.tx_var !== undefined && engine ? effective.tx_var : vals.tx_var;
    var fam = M.texTypes[Math.round(type)].family;
    plotCanvas(document.getElementById("wave"), function (t) { return Q.texWave(type, va, t); }, fam === 0 ? 1 : 2);
    document.getElementById("wavecap").textContent = Q.familyName(type) + " · " + Q.variationText({ tx_type: type, tx_var: va });
    plotCanvas(document.getElementById("lwave"), function (t) { return Q.lineWave(vals.ln_shape, vals.ln_var, t); }, 2);
    envCanvas(document.getElementById("txenv"), vals.tx_a, vals.tx_d, vals.tx_s, vals.tx_r);
    extraGraphics.forEach(function (f) { f(); });
  }

  /* ------------------------------------------------------------------ details tabs */
  var curTab = 0;
  var TABS = M.pages.filter(function (p) { return p.knobs && p.level !== "root"; });
  function buildTabs() {
    var tabs = document.getElementById("tabs");
    TABS.forEach(function (pg, i) {
      var b = document.createElement("button"); b.textContent = pg.label;
      b.onclick = function () { curTab = i; renderTab(); };
      tabs.appendChild(b);
    });
    renderTab();
  }
  function renderTab() {
    var body = document.getElementById("tabbody");
    body.innerHTML = "";
    extraGraphics = [];
    Array.prototype.forEach.call(document.getElementById("tabs").children, function (b, i) { b.classList.toggle("cur", i === curTab); });
    var pg = TABS[curTab], keys = pg.knobs;
    for (var row = 0; row < 2; row++) {
      var grp = document.createElement("div"); grp.className = "grp";
      var rowKeys = keys.slice(row * 4, row * 4 + 4);
      rowKeys.forEach(function (k) { if (k) grp.appendChild(ctl(k)); });
      var gfx = graphicFor(rowKeys);
      if (gfx) grp.appendChild(gfx);
      body.appendChild(grp);
    }
    redrawGraphics();
  }
  function graphicFor(keys) {
    var c = document.createElement("canvas"); c.width = 180; c.height = 64;
    var g = keys.map(function (k) { var v = k && byKey[k].viz; return v && v.group; }).filter(Boolean)[0];
    if (!g) return null;
    if (g === "me") extraGraphics.push(function () { envCanvas(c, vals.me_a, vals.me_d, vals.me_s, vals.me_r); });
    else if (g === "flt") extraGraphics.push(function () { filterCanvas(c, 20 + 115 * vals.flt_cut, vals.flt_res, byKey.flt_type.opts[vals.flt_type]); });
    else if (g === "lf") extraGraphics.push(function () { filterCanvas(c, 20 + 115 * vals.ln_cut, vals.ln_res, "LP24"); });
    else if (g === "an") extraGraphics.push(function () { lfoCanvas(c, vals.an_shape, vals.tx_anim); });
    else if (g === "drv") extraGraphics.push(function () {
      var amp = Math.round(vals.tx_drvpos) === 2, ref = amp ? 0.4 : 0.8, pre = amp ? 2.5 : 1;
      plotCanvas(c, function (t) { return Q.driveCurve(Math.sin(2 * Math.PI * t) * ref, vals.tx_drive, pre, ref) / ref; }, 2, "#d9625b");
    });
    else if (g === "det") extraGraphics.push(function () {
      var cx = cssCtx(c), W = c.width, H = c.height, bl = vals.tx_bloom;
      cx.clearRect(0, 0, W, H);
      var tb = bl < 0.005 ? 0 : (W - 8) * (0.08 + 0.85 * Math.sqrt(bl));
      for (var n = 0; n < 6; n++) {
        var cents = Q.detCents(n, vals.tx_detune);
        cx.strokeStyle = n % 2 ? "#b9ab8e" : "#e9e7e1"; cx.beginPath();
        for (var x = 0; x <= W - 8; x++) {
          var k = tb <= 0 ? 1 : Math.min(1, x / tb), y = H / 2 - cents / 45 * k * (H / 2 - 6);
          if (x) cx.lineTo(4 + x, y); else cx.moveTo(4 + x, y);
        }
        cx.stroke();
      }
    });
    else return null;
    return c;
  }

  /* ------------------------------------------------------------------ Move display simulator */
  var FONT = {
    A: "010101111101101", B: "110101110101110", C: "011100100100011", D: "110101101101110", E: "111100110100111",
    F: "111100110100100", G: "011100101101011", H: "101101111101101", I: "111010010010111", J: "001001001101010",
    K: "101101110101101", L: "100100100100111", M: "101111111101101", N: "110101101101101", O: "010101101101010",
    P: "110101110100100", Q: "010101101110011", R: "110101110101101", S: "011100010001110", T: "111010010010010",
    U: "101101101101111", V: "101101101101010", W: "101101111111101", X: "101101010101101", Y: "101101010010010",
    Z: "111001010100111", "0": "111101101101111", "1": "010110010010111", "2": "110001010100111", "3": "110001010001110",
    "4": "101101111001001", "5": "111100110001110", "6": "011100111101111", "7": "111001010010010", "8": "111101111101111",
    "9": "111101111001110", ".": "000000000000010", "-": "000000111000000", "+": "000010111010000", "/": "001001010100100",
    "%": "101001010100101", ":": "000010000010000", ">": "100010001010100", "<": "001010100010001", "#": "101111101111101",
    "&": "010101010101011", "(": "010100100100010", ")": "010001001001010", "?": "110001010000010", " ": "000000000000000",
    "!": "010010010000010", ",": "000000000010100", "'": "010010000000000", "=": "000111000111000", "*": "000101010101000",
    "±": "010111010000111", "_": "000000000000111",
  };
  var FB = new Uint8Array(128 * 64);
  function makeCtx(ox, oy, w, h) {
    function px(x, y, c) {
      x = Math.round(x); y = Math.round(y);
      if (x < 0 || y < 0 || x >= w || y >= h) return;
      var X = ox + x, Y = oy + y;
      if (X < 0 || Y < 0 || X >= 128 || Y >= 64) return;
      FB[Y * 128 + X] = c ? 1 : 0;
    }
    var ctx = {
      width: w, height: h,
      fillRect: function (x, y, ww, hh, c) {
        if (c === undefined) c = 1;
        for (var j = 0; j < hh; j++) for (var i = 0; i < ww; i++) px(x + i, y + j, c);
      },
      setPixel: function (x, y, c) { px(x, y, c === undefined ? 1 : c); },
      textWidth: function (s) { return Math.max(0, String(s).length * 4 - 1); },
      print: function (x, y, s, c) {
        s = String(s).toUpperCase();
        if (c === undefined) c = 1;
        for (var k = 0; k < s.length; k++) {
          var g = FONT[s[k]] || FONT["?"];
          for (var j = 0; j < 5; j++) for (var i = 0; i < 3; i++) if (g[j * 3 + i] === "1") px(x + k * 4 + i, y + 1 + j, c);
        }
      },
      line: function (x0, y0, x1, y1, c) {
        x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
        var dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (var n = 0; n < 512; n++) {
          px(x0, y0, c === undefined ? 1 : c);
          if (x0 === x1 && y0 === y1) break;
          var e2 = 2 * err;
          if (e2 >= dy) { err += dy; x0 += sx; }
          if (e2 <= dx) { err += dx; y0 += sy; }
        }
      },
      drawArc: function (cx, cy, r, start, sweep, c) {
        var n = Math.max(8, Math.ceil(Math.abs(sweep) / 6));
        for (var i = 0; i <= n; i++) {
          var a = (start + sweep * i / n) * Math.PI / 180;
          px(cx + Math.sin(a) * r, cy - Math.cos(a) * r, c === undefined ? 1 : c);
        }
      },
      drawCircle: function (cx, cy, r, c) { ctx.drawArc(cx, cy, r, 0, 360, c); },
      fillCircle: function (cx, cy, r, c) {
        for (var y = -r; y <= r; y++) { var xx = Math.round(Math.sqrt(r * r - y * y)); ctx.fillRect(cx - xx, cy + y, 2 * xx + 1, 1, c); }
      },
    };
    return ctx;
  }

  var page = 0;
  var GRID = M.pages;      // presets page included in the walk
  function currentKeys() { return GRID[page].knobs; }
  function groupsOf(keys) {
    // contiguous same-group cells within one row of four
    var out = [];
    for (var row = 0; row < 2; row++) {
      var i = 0;
      while (i < 4) {
        var k = keys[row * 4 + i], viz = k && byKey[k].viz, g = viz && viz.group;
        var span = 1;
        if (g && viz.span !== false) {
          while (i + span < 4) {
            var k2 = keys[row * 4 + i + span], v2 = k2 && byKey[k2].viz;
            if (v2 && v2.group === g && v2.span !== false) span++; else break;
          }
        }
        out.push({ row: row, col: i, span: span, key: k, group: span > 1 || (viz && viz.kind && viz.kind.indexOf("custom:") === 0) ? g : null });
        i += span;
      }
    }
    return out;
  }
  function liveValues() {
    var v = {};
    for (var k in vals) v[k] = vals[k];
    if (engine && effective.tx_var !== undefined) v["tx_var:effective"] = effective.tx_var;
    v.preset_name = M.presets[presetIndex].name;
    return v;
  }
  function drawDial(ctx, x, y, w, h, key) {
    var p = byKey[key], n = (vals[key] - p.mn) / (p.mx - p.mn);
    var cx = x + Math.floor(w / 2), cy = y + Math.floor(h / 2) + 1, r = Math.min(7, Math.floor(h / 2));
    for (var a = 210; a <= 510; a += 18) ctx.setPixel(cx + Math.sin(a * Math.PI / 180) * r, cy - Math.cos(a * Math.PI / 180) * r, 1);
    if (p.mn < 0) ctx.drawArc(cx, cy, r, 360, (n - 0.5) * 300, 1); else ctx.drawArc(cx, cy, r, 210, n * 300, 1);
    var ang = (210 + n * 300) * Math.PI / 180;
    ctx.line(cx, cy, cx + Math.sin(ang) * (r - 2), cy - Math.cos(ang) * (r - 2), 1);
  }
  function drawCell(cell, keys, values) {
    var x0 = cell.col * 32, y0 = 11 + cell.row * 26, w = cell.span * 32, wh = 17;
    var key = cell.key;
    if (!key) return;
    var p = byKey[key], viz = p.viz || {};
    var gctx = makeCtx(x0 + 1, y0, w - 2, wh);
    var kind = viz.kind;
    if (cell.group) {
      var gk = keys.filter(function (k) { return k && byKey[k].viz && byKey[k].viz.group === cell.group; });
      var gkind = gk.map(function (k) { return byKey[k].viz.kind; }).filter(Boolean)[0];
      if (gkind && gkind.indexOf("custom:") === 0) {
        try { canvas_overlay.widgetKinds[gkind](gctx, { values: values, group: { keys: gk } }); } catch (e) { console.warn(e); }
      } else if (gkind === "envelope") {
        var pre = cell.group;
        var hasS = vals[pre + "_s"] !== undefined, hasR = vals[pre + "_r"] !== undefined;
        Q.envShape(gctx, vals[pre + "_a"], vals[pre + "_d"], hasS ? vals[pre + "_s"] : 0, hasR ? vals[pre + "_r"] : 0, 0, 1, w - 2, wh - 2, -1);
      } else if (gkind === "filter") {
        var isL = cell.group === "lf";
        Q.filterCurve(gctx, 20 + 115 * vals[isL ? "ln_cut" : "flt_cut"], vals[isL ? "ln_res" : "flt_res"],
          isL ? "LP24" : byKey.flt_type.opts[vals.flt_type], 0, 1, w - 2, wh - 2);
      } else if (gkind === "lfo") {
        var sh = vals.an_shape, rnd = [0.3, -0.8, 0.6, -0.2, 0.9, -0.5, 0.1, -0.9];
        Q.plot(gctx, function (t) { var ph = t % 1; return sh === 0 ? Math.sin(2 * Math.PI * ph) : sh === 1 ? (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph) : sh === 2 ? 2 * ph - 1 : sh === 3 ? (ph < 0.5 ? 1 : -1) : rnd[Math.floor(t * 2) % 8]; }, 0, 1, w - 2, wh - 2, 2);
      }
    } else if (kind === "fader") {
      var fy = Math.round((1 - vals[key]) * (wh - 3));
      gctx.fillRect(Math.floor(w / 2) - 2, 0, 1, wh, 1);
      gctx.fillRect(Math.floor(w / 2) - 5, fy, 7, 3, 1);
    } else if (kind === "switch") {
      var onv = vals[key] >= 0.5, sx = Math.floor(w / 2) - 8;
      gctx.fillRect(sx, 5, 16, 1, 1); gctx.fillRect(sx, 11, 16, 1, 1); gctx.fillRect(sx, 5, 1, 7, 1); gctx.fillRect(sx + 15, 5, 1, 7, 1);
      gctx.fillRect(onv ? sx + 9 : sx + 2, 7, 5, 3, 1);
    } else if (p.kind === "e") {
      var t = (p.sopts || p.opts)[Math.round(vals[key])];
      var tw2 = gctx.textWidth(t) + 4, bx = Math.floor((w - 2 - tw2) / 2);
      gctx.fillRect(bx, 3, tw2, 1, 1); gctx.fillRect(bx, 11, tw2, 1, 1); gctx.fillRect(bx, 3, 1, 9, 1); gctx.fillRect(bx + tw2 - 1, 3, 1, 9, 1);
      gctx.print(bx + 2, 4, t, 1);
    } else if (p.kind === "i" && p.mx - p.mn <= 127) {
      var s = String(Math.round(vals[key]));
      gctx.print(Math.floor((w - 2 - gctx.textWidth(s)) / 2), 5, s, 1);
    } else {
      drawDial(gctx, 0, 0, w - 2, wh, key);
    }
    // labels: one per key in the span
    for (var i = 0; i < cell.span; i++) {
      var k = keys[cell.row * 4 + cell.col + i];
      if (!k) continue;
      var lc = makeCtx((cell.col + i) * 32, y0 + 18, 32, 7);
      var lab = byKey[k].short.toUpperCase();
      var isT = touched && touched.key === k && performance.now() - touched.at < 1500;
      if (isT) { lc.fillRect(0, 0, 32, 7, 1); lc.print(Math.floor((32 - lc.textWidth(lab)) / 2), 0, lab, 0); }
      else lc.print(Math.floor((32 - lc.textWidth(lab)) / 2), 0, lab, 1);
    }
  }
  function drawOled() {
    FB.fill(0);
    var full = makeCtx(0, 0, 128, 64), pg = GRID[page], values = liveValues();
    var now = performance.now(), t = touched && now - touched.at < 1500 ? touched : null;
    if (t && pg.knobs && pg.knobs.indexOf(t.key) < 0) t = null;
    if (!pg.knobs) {
      full.print(1, 0, "PRESETS", 1);
      var pos = (presetIndex + 1) + "/" + M.presets.length;
      full.print(127 - full.textWidth(pos), 0, pos, 1);
      for (var i = 0; i < 128; i += 2) full.setPixel(i, 8, 1);
      for (var r = -2; r <= 2; r++) {
        var idx = presetIndex + r; if (idx < 0 || idx >= M.presets.length) continue;
        var y = 26 + r * 9, nm = M.presets[idx].name.toUpperCase();
        if (r === 0) { full.fillRect(0, y - 1, 128, 8, 1); full.print(3, y, nm, 0); } else full.print(3, y, nm, 1);
      }
    } else {
      var head = t ? byKey[t.key].name : pg.label, hv = t ? Q.describe(t.key, values) : (page + 1) + "/" + GRID.length;
      full.print(1, 0, head, 1);
      full.print(127 - full.textWidth(hv), 0, hv, 1);
      for (var i2 = 0; i2 < 128; i2 += 2) full.setPixel(i2, 8, 1);
      groupsOf(pg.knobs).forEach(function (c) { drawCell(c, pg.knobs, values); });
      // card overlay while a carded knob is being turned
      if (t && now - t.at < 1200 && byKey[t.key]) {
        var cw = 104, ch = 40, cx = 12, cy = 14;
        full.fillRect(cx - 2, cy - 2, cw + 4, ch + 4, 0);
        full.fillRect(cx - 2, cy - 2, cw + 4, 1, 1); full.fillRect(cx - 2, cy + ch + 1, cw + 4, 1, 1);
        full.fillRect(cx - 2, cy - 2, 1, ch + 4, 1); full.fillRect(cx + cw + 1, cy - 2, 1, ch + 4, 1);
        var cctx = makeCtx(cx, cy, cw, ch);
        var hasCard = chainHasCard(t.key);
        try {
          if (hasCard) quark_card(cctx, { name: byKey[t.key].name, value: Q.describe(t.key, values), raw: vals[t.key], values: values, w: cw, h: ch, nowMs: now });
          else { cctx.print(1, 0, byKey[t.key].name, 1); cctx.print(1, 16, Q.describe(t.key, values), 1); }
        } catch (e) { console.warn(e); }
      }
    }
    var cv = document.getElementById("oled"), c = cv.getContext("2d");
    var img = c.createImageData(128, 64);
    for (var j = 0; j < 128 * 64; j++) {
      var on2 = FB[j];
      img.data[j * 4] = on2 ? 207 : 4; img.data[j * 4 + 1] = on2 ? 227 : 6; img.data[j * 4 + 2] = on2 ? 255 : 10; img.data[j * 4 + 3] = 255;
    }
    var tmp = drawOled.tmp || (drawOled.tmp = document.createElement("canvas"));
    tmp.width = 128; tmp.height = 64; tmp.getContext("2d").putImageData(img, 0, 0);
    c.imageSmoothingEnabled = false;
    c.drawImage(tmp, 0, 0, 512, 256);
  }
  function chainHasCard(key) { return !!byKey[key].card; }

  function buildMove() {
    var enc = document.getElementById("encoders");
    for (var i = 0; i < 8; i++) {
      (function (i) {
        var e = document.createElement("div"); e.className = "enc";
        var k = document.createElement("div"); k.className = "k"; k.innerHTML = "<i></i>";
        var l = document.createElement("div");
        e.append(k, l); enc.appendChild(e);
        var y0 = 0, acc = 0;
        function keyAt() { var ks = currentKeys(); return ks ? ks[i] : null; }
        function bump(steps, fine) {
          var key = keyAt();
          if (!key) { if (!GRID[page].knobs && i === 0) loadPreset(presetIndex + steps); return; }
          var p = byKey[key];
          var st = p.kind === "f" ? (p.mx - p.mn) * (fine ? 0.002 : 0.01) : 1;
          setParam(key, vals[key] + steps * st, true);
        }
        k.addEventListener("pointerdown", function (ev) {
          k.setPointerCapture(ev.pointerId); y0 = ev.clientY; acc = 0;
          var key = keyAt(); if (key) touched = { key: key, at: performance.now() };
        });
        k.addEventListener("pointermove", function (ev) {
          if (!(ev.buttons & 1)) return;
          acc += (y0 - ev.clientY); y0 = ev.clientY;
          var steps = Math.trunc(acc / 4);
          if (steps) { acc -= steps * 4; bump(steps, ev.shiftKey); }
        });
        k.addEventListener("wheel", function (ev) { ev.preventDefault(); bump(ev.deltaY < 0 ? 1 : -1, ev.shiftKey); }, { passive: false });
        e.update = function () {
          var key = keyAt();
          l.textContent = key ? byKey[key].short : (!GRID[page].knobs && i === 0 ? "Preset" : "");
          var p = key && byKey[key];
          var n = p ? (vals[key] - p.mn) / (p.mx - p.mn) : 0;
          k.firstChild.style.transform = "rotate(" + (-135 + 270 * n) + "deg)";
          e.classList.toggle("touch", !!(touched && key && touched.key === key && performance.now() - touched.at < 1500));
        };
      })(i);
    }
    var pages = document.getElementById("pages");
    GRID.forEach(function (pg, i) {
      var b = document.createElement("button"); b.textContent = pg.label;
      b.onclick = function () { page = i; touched = null; };
      pages.appendChild(b);
    });
    document.getElementById("jprev").onclick = function () { page = (page - 1 + GRID.length) % GRID.length; touched = null; };
    document.getElementById("jnext").onclick = function () { page = (page + 1) % GRID.length; touched = null; };
  }
  function tickMove() {
    document.getElementById("pagename").textContent = GRID[page].label;
    Array.prototype.forEach.call(document.getElementById("pages").children, function (b, i) { b.classList.toggle("cur", i === page); });
    Array.prototype.forEach.call(document.getElementById("encoders").children, function (e) { e.update(); });
    drawOled();
  }

  /* ------------------------------------------------------------------ keyboard + MIDI */
  var octave = 4, vel = 100, held = {}, sustain = false, running = false;
  var kbLatch = !!(window.matchMedia && window.matchMedia("(pointer: coarse)").matches), latched = {};
  var KEYMAP = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11, k: 12, o: 13, l: 14, p: 15, ";": 16, "'": 17 };
  function keyEl(n) { return document.querySelector('[data-n="' + n + '"]'); }
  function noteOn(n, v) {
    if (engine) engine.midi([0x90, n, v || vel]);
    else if (!starting) document.getElementById("power").click();   /* first tap starts the audio */
    var el = keyEl(n); if (el) el.classList.add("down");
  }
  function noteOff(n) { if (engine) engine.midi([0x80, n, 0]); var el = keyEl(n); if (el) el.classList.remove("down"); }
  /* Latch: a tap holds the note until it is tapped again (or Release all) - chords on a phone */
  function latchToggle(n) {
    if (latched[n]) { delete latched[n]; noteOff(n); }
    else { latched[n] = 1; noteOn(n); }
    var el = keyEl(n); if (el) el.classList.toggle("latched", !!latched[n]);
  }
  function releaseLatched() {
    Object.keys(latched).forEach(function (n) { noteOff(+n); var el = keyEl(+n); if (el) el.classList.remove("latched"); });
    latched = {};
  }
  function setKbLatch(onv) {
    kbLatch = onv;
    document.getElementById("kblatch").classList.toggle("on", onv);
    if (!onv) releaseLatched();
  }
  function buildPiano() {
    var el = document.getElementById("piano"); el.innerHTML = "";
    var width = el.clientWidth || 1000, small = width < 700;
    var ww = small ? Math.max(34, Math.floor(width / 14)) : Math.max(18, Math.floor(width / 21));
    var whites = Math.max(7, Math.floor(width / ww)), first = 12 * (octave - 1), wi = 0;
    for (var n = first; n <= 127 && wi < whites; n++) {
      var pc = n % 12, black = [1, 3, 6, 8, 10].indexOf(pc) >= 0;
      if (black && wi >= whites) break;
      var k = document.createElement("div");
      k.className = black ? "b" : "w"; k.dataset.n = n;
      if (black) { k.style.left = (wi * ww - ww * 0.3) + "px"; k.style.width = (ww * 0.6) + "px"; }
      else { k.style.left = (wi * ww) + "px"; k.style.width = ww + "px"; if (pc === 0) k.innerHTML = "<em>" + Q.noteName(n) + "</em>"; wi++; }
      if (latched[n]) k.classList.add("latched", "down");
      (function (n, k) {
        var downNow = false;
        k.addEventListener("pointerdown", function (e) {
          e.preventDefault();
          if (kbLatch) { latchToggle(n); return; }
          downNow = true; noteOn(n);
          try { k.setPointerCapture(e.pointerId); } catch (err) { /* synthetic events */ }
        });
        function up() { if (downNow) { downNow = false; noteOff(n); } }
        k.addEventListener("pointerup", up);
        k.addEventListener("pointercancel", up);
        k.addEventListener("contextmenu", function (e) { e.preventDefault(); });
      })(n, k);
      el.appendChild(k);
    }
    document.getElementById("octv").textContent = Q.noteName(12 * octave);
  }
  function inField(e) { return e.target && (e.target.tagName === "INPUT" || e.target.tagName === "SELECT"); }
  window.addEventListener("keydown", function (e) {
    if (inField(e) || e.metaKey || e.ctrlKey) return;
    var k = e.key.toLowerCase();
    if (k === "[") { page = (page - 1 + GRID.length) % GRID.length; return; }
    if (k === "]") { page = (page + 1) % GRID.length; return; }
    if (e.repeat) return;
    if (k === "z") { octave = Math.max(1, octave - 1); buildPiano(); return; }
    if (k === "x") { octave = Math.min(8, octave + 1); buildPiano(); return; }
    if (k === "c") { vel = Math.max(1, vel - 20); document.getElementById("vel").value = vel; return; }
    if (k === "v") { vel = Math.min(127, vel + 20); document.getElementById("vel").value = vel; return; }
    if (k === " ") { e.preventDefault(); setSustain(true); return; }
    if (KEYMAP[k] !== undefined) {
      var n = 12 * octave + KEYMAP[k];
      if (kbLatch) { latchToggle(n); return; }
      if (!held[k]) { held[k] = n; noteOn(n); }
    }
  });
  window.addEventListener("keyup", function (e) {
    var k = e.key.toLowerCase();
    if (k === " ") { setSustain(false); return; }
    if (held[k] !== undefined) { noteOff(held[k]); delete held[k]; }
  });
  function setSustain(onv) {
    sustain = onv;
    document.getElementById("hold").classList.toggle("on", onv);
    if (engine) engine.midi([0xB0, 64, onv ? 127 : 0]);
  }
  function initMidi() {
    var st = document.getElementById("midi");
    if (!navigator.requestMIDIAccess) { st.textContent = "n/a"; return; }
    navigator.requestMIDIAccess().then(function (acc) {
      function hook() {
        var n = 0;
        acc.inputs.forEach(function (inp) {
          n++;
          inp.onmidimessage = function (m) {
            var d = m.data; if (!engine || d.length < 1 || d[0] >= 0xF0) return;
            engine.midi([d[0], d[1] || 0, d[2] || 0]);
            var s = d[0] & 0xF0;
            if (s === 0x90 && d[2]) { var el = document.querySelector('[data-n="' + d[1] + '"]'); if (el) el.classList.add("down"); }
            if (s === 0x80 || (s === 0x90 && !d[2])) { var el2 = document.querySelector('[data-n="' + d[1] + '"]'); if (el2) el2.classList.remove("down"); }
          };
        });
        st.textContent = n ? n + " in" : "none";
      }
      hook(); acc.onstatechange = hook;
    }, function () { st.textContent = "denied"; });
  }

  /* ------------------------------------------------------------------ meters */
  function onStat(d) {
    document.getElementById("peak").style.width = Math.min(100, d.peak * 100) + "%";
  }
  /* Benchmark: a private engine instance renders 3 s of the CURRENT patch with an 8-note chord on the
   * Pads and a Mono line (the "1 of 8 tracks" budget), timed on this thread. */
  function measureCpu() {
    var el = document.getElementById("load");
    el.textContent = "…";
    setTimeout(function () {
      var x = new WebAssembly.Instance(new WebAssembly.Module(b64ToBytes(QUARK_WASM_B64)), {}).exports;
      if (x._initialize) x._initialize();
      x.wa_init(44100);
      function put(ptr, cap, s) { var m = new Uint8Array(x.memory.buffer), b = new TextEncoder().encode(s), n = Math.min(b.length, cap - 1); m.set(b.subarray(0, n), ptr); m[ptr + n] = 0; }
      function set(k, v) { put(x.wa_key(), x.wa_key_cap(), k); put(x.wa_val(), x.wa_val_cap(), String(v)); x.wa_set(); }
      set("state", stateJson());
      if (vals.ln_vol < 0.05) set("ln_vol", 0.6);
      if (vals.tx_vol < 0.05) set("tx_vol", 0.8);
      set("key_mode", 0); set("ln_mode", 1); set("tx_voices", Math.max(8, vals.tx_voices));
      [48, 55, 60, 64, 67, 71, 74, 79].forEach(function (n) { x.wa_midi(0x90, n, 100); });
      for (var w = 0; w < 100; w++) x.wa_render(128);
      var blocks = Math.round(3 * 44100 / 128), t0 = performance.now();
      for (var i = 0; i < blocks; i++) {
        if (i % 86 === 0) { x.wa_midi(0x90, 36 + (i / 86 % 5) * 2, 100); }
        x.wa_render(128);
      }
      var ms = performance.now() - t0, pct = ms / 3000 * 100;
      el.textContent = pct.toFixed(2) + "% here";
      el.title = "8 pad voices + a Mono line + all FX of this patch, single-threaded WebAssembly on this computer. " +
        "A Move (Cortex-A72 @1.5 GHz, native code) is very roughly 4-6x slower than a recent laptop core: ~" +
        (pct * 4).toFixed(0) + "-" + (pct * 6).toFixed(0) + "% estimated. Measure on the device with the profile build for the real number.";
    }, 30);
  }
  var scopeBuf = new Float32Array(1024);
  function drawScope() {
    var cv = document.getElementById("scope"), c = cv.getContext("2d");
    c.clearRect(0, 0, cv.width, cv.height);
    if (!engine) return;
    engine.an.getFloatTimeDomainData(scopeBuf);
    var start = 0;
    for (var i = 1; i < 512; i++) if (scopeBuf[i - 1] < 0 && scopeBuf[i] >= 0) { start = i; break; }
    c.strokeStyle = "#b9ab8e"; c.beginPath();
    for (var x = 0; x < cv.width; x++) {
      var y = cv.height / 2 - scopeBuf[start + x * 2] * cv.height * 0.45;
      if (x) c.lineTo(x, y); else c.moveTo(x, y);
    }
    c.stroke();
  }
  var frame = 0;
  function loop() {
    frame++;
    if (engine && frame % 3 === 0) {
      engine.get("tx_var:effective").then(function (v) {
        var x = parseFloat(v);
        if (isFinite(x) && Math.abs((effective.tx_var || 0) - x) > 0.002) { effective.tx_var = x; redrawGraphics(); }
      });
    }
    if (engine && frame % 15 === 0) engine.get("voices").then(function (v) { if (v) document.getElementById("voices").textContent = v.replace(",", " pads · ") + " mono"; });
    if (engine && frame % 10 === 0) engine.get("arp_notes").then(function (v) {
      var t = vals.arp_mode < 0.5 ? "" : v ? "ARP ▸ " + v.split(",").map(function (x) { return Q.noteName(+x); }).join(" ") : "ARP ▸ waiting for " + (vals.arp_src >= 0.5 ? "a pad chord" : "Mono keys");
      document.getElementById("arpnotes").textContent = t;
    });
    tickMove();
    drawScope();
    requestAnimationFrame(loop);
  }

  /* ------------------------------------------------------------------ boot */
  function boot() {
    ["tx_type", "tx_anim", "tx_depth", "tx_a", "tx_d", "tx_s", "tx_r", "ens_amt", "ens_type",
     "ln_shape", "ln_var", "ln_a", "ln_d", "ln_sus", "ln_spread", "ln_dly", "ln_dtime", "fx_type", "fx_ctrl", "fx_amt", "sh_pitch", "sh_amt",
     "rv_decay", "rv_amt", "tx_drive", "tx_drvpos", "tx_detune", "tx_bloom", "tx_latch", "ln_wenv", "arp_mode", "arp_src",
     "arp_oct", "arp_hold"].forEach(function (k) { mount(k); });
    mount("tx_var", "beige");
    mount("tx_vol", "white"); mount("ln_vol", "white"); mount("vol", "white"); mount("vintage", "beige");
    var sel = document.getElementById("preset");
    M.presets.forEach(function (p, i) { var o = document.createElement("option"); o.value = i; o.textContent = (i + 1) + "  " + p.name; sel.appendChild(o); });
    sel.onchange = function () { loadPreset(+sel.value); };
    document.getElementById("pprev").onclick = function () { loadPreset(presetIndex - 1); };
    document.getElementById("pnext").onclick = function () { loadPreset(presetIndex + 1); };
    document.getElementById("power").onclick = async function () {
      var b = this;
      if (engine) { if (engine.ctx.state === "running") { await engine.ctx.suspend(); b.textContent = "⏻ Resume audio"; } else { await engine.ctx.resume(); b.textContent = "⏻ Audio on"; } return; }
      if (starting) return;
      starting = true;
      b.textContent = "Starting…";
      try {
        await Engine.start(); b.textContent = "⏻ Audio on"; b.classList.add("live");
        Object.keys(latched).forEach(function (n) { engine.midi([0x90, +n, vel]); });
      } catch (e) { b.textContent = "Audio failed: " + e.message; console.error(e); }
      starting = false;
    };
    document.getElementById("octdn").onclick = function () { octave = Math.max(1, octave - 1); buildPiano(); };
    document.getElementById("octup").onclick = function () { octave = Math.min(8, octave + 1); buildPiano(); };
    document.getElementById("vel").oninput = function () { vel = +this.value; };
    document.getElementById("wheel").oninput = function () { if (engine) engine.midi([0xB0, 1, +this.value]); };
    var bend = document.getElementById("bend");
    bend.oninput = function () { var v = +this.value + 8192; if (engine) engine.midi([0xE0, v & 127, v >> 7]); };
    bend.onchange = function () { this.value = 0; this.oninput(); };
    document.getElementById("hold").onclick = function () { setSustain(!sustain); };
    document.getElementById("panic").onclick = function () {
      latched = {};
      if (engine) engine.midi([0xB0, 123, 0]);
      document.querySelectorAll(".down, .latched").forEach(function (e) { e.classList.remove("down", "latched"); });
    };
    document.getElementById("kblatch").onclick = function () { setKbLatch(!kbLatch); };
    document.getElementById("kbclear").onclick = releaseLatched;
    setKbLatch(kbLatch);
    document.getElementById("bpm").onchange = function () { if (engine) engine.tempo(+this.value || 120, running); };
    document.getElementById("run").onclick = function () { running = !running; this.classList.toggle("on", running); if (engine) engine.tempo(+document.getElementById("bpm").value || 120, running); };
    document.getElementById("load").onclick = measureCpu;
    buildTabs();
    buildMove();
    buildPiano();
    var rz = 0;
    window.addEventListener("resize", function () { clearTimeout(rz); rz = setTimeout(buildPiano, 150); });
    initMidi();
    loadPreset(0);
    loop();
  }
  boot();
})();
