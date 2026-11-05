/* Quark: module-drawn knob-cell widgets (Schwung `drawCell`, docs/MODULES.md "Custom widgets").
 *
 *  custom:qwave  Type + Variation as ONE picture: the actual single cycle the Atmos engine plays
 *                for that type at that Variation (string registers, wavetable frame, FM index). The
 *                Variation key is `live`, so Motion shows as a moving wave. A tick under the
 *                wave marks Variation; three dots name the family (1 strings, 2 wavetable, 3 FM).
 *  custom:qline  Melody Shape + Variation: the oscillator shape (with the PWM the macro adds) and a
 *                three-segment meter for Variation's PWM -> tremolo -> vibrato stages; for a wavetable
 *                shape, the wave at that position and a position tick.
 *  custom:qdrive Atmos Drive + Position: the transfer curve the drive applies (it bends and flattens
 *                as Drive rises), a sine through it, and PRE / POST / AMP.
 *  custom:qdet   Atmos Detune + Bloom: the notes of a chord as lines that start together and fan out
 *                to their detune offsets over the Bloom time.
 *                With Type Off the wave is a dotted line; a "+" in the corner says a Layer sounds on top.
 *  custom:qlab   Layer A + B + Blend: the two sets' level outlines, the louder one solid, a Blend
 *                tick, and the Layer's "+" (added on top of the Type).
 *  custom:qpcm   Layer Start + Mode: the sounding set's outline, a bracket over its loop, a Start
 *                tick and a Mode glyph (a loop arrow or a one-shot bar; boxed for the post-filter
 *                modes). Off: a dotted flat line.
 *  custom:qera   Layer Era: a sine drawn the way that Era reads the ROM (smooth / coarse / stepped).
 *  custom:qflt   Atmos Cutoff + Resonance: the response of the selected Filter Type (read from the
 *                page's values), including the combs' teeth, Dual BP's two bands and Morph's blend.
 *
 * (0,0) is the cell box, ctx.width/height its size; values arrive, never read (no getParam).
 */
(function () {
  /* BEGIN GENERATED */
  /* END GENERATED */

  /* the "+" that marks the Layer: samples added on top of the Atmos Type (5x5 at x, y) */
  function plus(ctx, x, y) {
    ctx.fillRect(x, y, 5, 5, 0);
    ctx.fillRect(x, y + 2, 5, 1, 1);
    ctx.fillRect(x + 2, y, 1, 5, 1);
  }

  function drawWave(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var type = Q.val(v, "tx_type"), va = Q.val(v, "tx_var");
    var fam = Q.M.texTypes[Math.max(0, Math.min(Q.M.texTypes.length - 1, Math.round(type)))].family;
    var wh = Math.max(6, H - 4);
    if (fam === 3) Q.dotted(ctx, 0, Math.floor(wh / 2), W);   /* Off: no oscillator */
    else Q.plot(ctx, function (t) { return Q.texWave(type, va, t); }, 0, 0, W, wh, fam === 0 ? 1 : 2);
    if (Q.plSet(v)) plus(ctx, 0, 0);   /* a Layer sounds on top */
    /* variation tick + family dots */
    var by = H - 2;
    Q.dotted(ctx, 0, by, W);
    var mx = Math.round(Math.max(0, Math.min(1, va)) * (W - 1));
    ctx.fillRect(Math.max(0, mx - 1), by - 1, 3, 3, 1);
    for (var i = 0; i <= Math.min(fam, 2); i++) if (fam < 3) ctx.fillRect(W - 2 - i * 3, 0, 2, 2, 1);
  }

  function drawLine(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var shape = Q.val(v, "ln_shape"), va = Q.val(v, "ln_var");
    var wh = Math.max(6, H - 5);
    Q.plot(ctx, function (t) { return Q.lineWave(shape, va, t); }, 0, 0, W, wh, 2);
    if (Q.lineIsWave(shape)) {
      var by = H - 2, mx = Math.round(Math.max(0, Math.min(1, va)) * (W - 1));
      Q.dotted(ctx, 0, by, W);
      ctx.fillRect(Math.max(0, mx - 1), by - 1, 3, 3, 1);
      return;
    }
    var seg = Math.floor((W - 4) / 3), y = H - 3;
    for (var s = 0; s < 3; s++) {
      var fill = Math.max(0, Math.min(1, va * 3 - s));
      var x = s * (seg + 2);
      ctx.fillRect(x, y, seg, 1, 1);
      if (fill > 0) ctx.fillRect(x, y - 1, Math.max(1, Math.round(seg * fill)), 3, 1);
    }
  }

  function drawDrive(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var amt = Q.val(v, "tx_drive"), pos = Math.round(Q.val(v, "tx_drvpos"));
    var amp = pos === 2, s = Math.min(H - 1, Math.floor(W * 0.45));
    /* transfer curve in a square on the left */
    Q.dotted(ctx, 0, Math.floor(s / 2), s);
    var px = null, py = null;
    for (var i = 0; i < s; i++) {
      var x = (i / (s - 1)) * 2 - 1, y = Q.driveCurve(x * (amp ? 0.4 : 0.8), amt, amp ? 2.5 : 1, amp ? 0.4 : 0.8) / (amp ? 0.4 : 0.8);
      var yy = Math.round((0.5 - 0.5 * Math.max(-1, Math.min(1, y))) * (s - 1));
      if (px !== null) Q.line(ctx, px, py, i, yy);
      px = i; py = yy;
    }
    /* a sine through it on the right, with the position name */
    var x0 = s + 3, w = W - x0;
    Q.plot(ctx, function (t) {
      var x = Math.sin(2 * Math.PI * t) * (amp ? 0.4 : 0.8);
      return Q.driveCurve(x, amt, amp ? 2.5 : 1, amp ? 0.4 : 0.8) / (amp ? 0.4 : 0.8);
    }, x0, 0, w, H - 7, 1);
    var lab = ["PRE", "POST", "AMP"][pos] || "";
    ctx.print(x0, H - 7, lab, 1);
  }

  function drawDetune(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var det = Q.val(v, "tx_detune"), bl = Q.val(v, "tx_bloom");
    var cy = Math.floor(H / 2), span = Math.max(1, H / 2 - 1);
    var t0 = 3, tb = bl < 0.005 ? 0 : Math.min(W - t0 - 2, Math.round((W - t0) * (0.08 + 0.85 * Math.sqrt(bl))));
    ctx.fillRect(0, cy, 2, 1, 1);
    for (var n = 0; n < 4; n++) {
      var c = Q.detCents(n, det) / 45, py = cy;
      for (var x = t0; x < W; x++) {
        var k = tb <= 0 ? 1 : Math.min(1, (x - t0) / tb);
        var y = Math.round(cy - c * k * span);
        if (x > t0) Q.line(ctx, x - 1, py, x, y);
        py = y;
      }
    }
  }

  function drawFilter(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var opts = Q.byKey.flt_type.opts, ti = Math.max(0, Math.min(opts.length - 1, Math.round(Q.val(v, "flt_type"))));
    var type = opts[ti];
    var fc = Q.hz(20 + 115 * Q.val(v, "flt_cut")), res = Q.val(v, "flt_res");
    var bot = H - 1;
    Q.dotted(ctx, 0, bot, W);
    var py = null;
    for (var x = 0; x < W; x++) {
      var f = 20 * Math.pow(1000, x / Math.max(1, W - 1));
      var db = 20 * Math.log(Math.max(1e-4, Q.filterMag(type, f, fc, res))) / Math.LN10;
      var y = Math.round((1 - Math.max(0, Math.min(1, (db + 30) / 48))) * (H - 2));
      if (py !== null) Q.line(ctx, x - 1, py, x, y);
      py = y;
    }
  }

  /* the Mode glyph, 7x5 at (x, y): Loop / Air an arrow curling back, Shot / Ping a bar with a stop;
   * Air / Ping (after the filter) are boxed */
  function modeGlyph(ctx, x, y, mode) {
    if (mode === 0 || mode === 2) {
      ctx.fillRect(x + 1, y, 5, 1, 1); ctx.fillRect(x + 6, y, 1, 4, 1); ctx.fillRect(x + 1, y + 4, 6, 1, 1);
      ctx.fillRect(x, y + 3, 1, 3, 1); ctx.fillRect(x + 1, y + 4, 1, 1, 1);
    } else {
      ctx.fillRect(x, y + 2, 5, 1, 1); ctx.fillRect(x + 6, y, 1, 5, 1);
    }
    if (mode >= 2) { Q.dotted(ctx, x - 1, y - 2, 8); Q.dotted(ctx, x - 1, y + 6, 8); }
  }

  function drawPcm(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var st = Q.plSet(v), mode = Math.max(0, Math.min(3, Math.round(Q.val(v, "pl_mode"))));
    var gw = W >= 40 ? 11 : 0;   /* room for the Mode glyph in a wide (grouped) cell */
    var w = W - gw, h = H - 3;
    if (!st) { Q.dotted(ctx, 0, Math.floor(h / 2), w); }
    else {
      Q.pcmOutline(ctx, st, 0, 0, w, h, w >= 40 ? 2 : 1);
      if (st.loop) {   /* loop bracket along the top */
        var lx = Math.round(st.loop[0] * (w - 1));
        ctx.fillRect(lx, 0, 1, 3, 1); ctx.fillRect(w - 1, 0, 1, 3, 1); ctx.fillRect(lx, 0, w - lx, 1, 1);
      }
    }
    var by = H - 2, sx = st ? Math.round(Q.plStartFrac(st, Q.val(v, "pl_start")) * (w - 1)) : 0;
    Q.dotted(ctx, 0, by, w);
    ctx.fillRect(Math.max(0, sx - 1), by - 1, 3, 3, 1);
    if (gw) modeGlyph(ctx, W - 8, Math.max(2, Math.floor((H - 5) / 2)), mode);
  }

  /* Layer A + B + Blend (one group): A's outline on top, B's below, each solid while it is the louder
   * side and sparse while it is the quieter; a tick on the baseline marks Blend. One side Off: the
   * other alone, full height. */
  function drawLab(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var a = Q.plSet(v, 0), b = Q.plSet(v, 1), bl = Math.max(0, Math.min(1, Q.val(v, "pl_blend")));
    var x0 = 7, h = H - 3;
    plus(ctx, 0, 0);
    if (!a && !b) { Q.dotted(ctx, x0, Math.floor(h / 2), W - x0); return; }
    /* each set's short name left of its outline, so A and B read without touching a knob */
    var lw = Math.max(a ? Q.tw(ctx, a.short) : 0, b ? Q.tw(ctx, b.short) : 0) + 3;
    var named = W - x0 - lw >= 24 && (!a || !b || h >= 14), xo = named ? x0 + lw : x0, w = W - xo;
    function name(y, t) { if (named) ctx.print(x0, y, t, 1); }   /* too narrow: outlines only */
    if (!a || !b) {
      name(Math.max(0, Math.floor((h - 7) / 2)), (a || b).short);
      Q.pcmOutline(ctx, a || b, xo, 0, w, h, 1);
    } else {
      var rh = Math.floor(h / 2);
      name(Math.max(0, Math.floor((rh - 7) / 2)), a.short);
      name(rh + Math.max(0, Math.floor((rh - 7) / 2)), b.short);
      Q.pcmOutline(ctx, a, xo, 0, w, rh - 1, bl <= 0.5 ? 1 : 3);
      Q.pcmOutline(ctx, b, xo, rh, w, rh - 1, bl > 0.5 ? 1 : 3);
      var by = H - 2, mx = xo + Math.round(bl * (w - 1));
      Q.dotted(ctx, xo, by, w);
      ctx.fillRect(Math.max(xo, mx - 1), by - 1, 3, 3, 1);
    }
  }

  function drawEra(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var era = Math.max(0, Math.min(2, Math.round(Q.val(v, "pl_era"))));
    Q.plot(ctx, function (t) { return Q.eraWave(era, t); }, 0, 0, W, H - 1, 1);
  }

  globalThis.canvas_overlay = {
    widgetKinds: {
      "custom:qwave": drawWave,
      "custom:qline": drawLine,
      "custom:qdrive": drawDrive,
      "custom:qdet": drawDetune,
      "custom:qflt": drawFilter,
      "custom:qpcm": drawPcm,
      "custom:qera": drawEra,
      "custom:qlab": drawLab,
    },
  };
})();
