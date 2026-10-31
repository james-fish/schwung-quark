/* Quark: module-drawn knob-cell widgets (Schwung `drawCell`, docs/MODULES.md "Custom widgets").
 *
 *  custom:qwave  Type + Variation as ONE picture: the actual single cycle the Pads engine plays
 *                for that type at that Variation (string registers, wavetable frame, FM index). The
 *                Variation key is `live`, so Animate's motion shows as a moving wave. A tick under the
 *                wave marks Variation; three dots name the family (1 strings, 2 wavetable, 3 FM).
 *  custom:qline  Mono Shape + Variation: the oscillator shape (with the PWM the macro adds) and a
 *                three-segment meter for Variation's PWM -> tremolo -> vibrato stages; for a wavetable
 *                shape, the wave at that position and a position tick.
 *  custom:qdrive Pads Drive + Position: the transfer curve the drive applies (it bends and flattens
 *                as Drive rises), a sine through it, and PRE / POST / AMP.
 *  custom:qdet   Pads Detune + Bloom: the notes of a chord as lines that start together and fan out
 *                to their detune offsets over the Bloom time.
 *  custom:qflt   Pads Cutoff + Resonance: the response of the selected Filter Type (read from the
 *                page's values), including the combs' teeth, Dual BP's two bands and Morph's blend.
 *
 * (0,0) is the cell box, ctx.width/height its size; values arrive, never read (no getParam).
 */
(function () {
  /* BEGIN GENERATED */
  /* END GENERATED */

  function drawWave(ctx, o) {
    var W = ctx.width, H = ctx.height, v = o.values || {};
    var type = Q.val(v, "tx_type"), va = Q.val(v, "tx_var");
    var fam = Q.M.texTypes[Math.max(0, Math.min(Q.M.texTypes.length - 1, Math.round(type)))].family;
    var wh = Math.max(6, H - 4);
    Q.plot(ctx, function (t) { return Q.texWave(type, va, t); }, 0, 0, W, wh, fam === 0 ? 1 : 2);
    /* variation tick + family dots */
    var by = H - 2;
    Q.dotted(ctx, 0, by, W);
    var mx = Math.round(Math.max(0, Math.min(1, va)) * (W - 1));
    ctx.fillRect(Math.max(0, mx - 1), by - 1, 3, 3, 1);
    for (var i = 0; i <= fam; i++) ctx.fillRect(W - 2 - i * 3, 0, 2, 2, 1);
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

  globalThis.canvas_overlay = {
    widgetKinds: {
      "custom:qwave": drawWave,
      "custom:qline": drawLine,
      "custom:qdrive": drawDrive,
      "custom:qdet": drawDetune,
      "custom:qflt": drawFilter,
    },
  };
})();
