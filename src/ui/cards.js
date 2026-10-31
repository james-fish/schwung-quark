/* Quark: parameter cards (Schwung `card_script`), raised while a knob is held or turning.
 *
 * One drawer serves every carded knob. The header line is the parameter and its value in real units
 * (seconds, Hz, semitones, register names); the body is a small picture of what the knob does.
 * Only knobs whose picture adds something the page does not already show get a card: the pad wave,
 * the Mono shape, the ensemble, the shimmer tail, the detune fan, the arp contours and dice, the
 * split. Plain amounts use the host's own value display. o.raw may be null: then only the name is drawn.
 */
(function () {
  /* BEGIN GENERATED */
  /* END GENERATED */

  function header(ctx, o, label, text) {
    var W = o.w;
    var t = Q.fit(ctx, text, W - 2);
    var lw = W - Q.tw(ctx, t) - 6;
    ctx.print(1, 0, Q.fit(ctx, label, Math.max(12, lw)), 1);
    ctx.print(W - 1 - Q.tw(ctx, t), 0, t, 1);
    Q.dotted(ctx, 0, 9, W);
  }

  function bar(ctx, x, y, w, v, bipolar) {
    ctx.fillRect(x, y + 2, w, 1, 1);
    if (bipolar) {
      var c = x + Math.round(w / 2), e = x + Math.round((v * 0.5 + 0.5) * (w - 1));
      ctx.fillRect(c, y, 1, 5, 1);
      ctx.fillRect(Math.min(c, e), y + 1, Math.abs(e - c) + 1, 3, 1);
    } else {
      ctx.fillRect(x, y + 1, Math.max(1, Math.round(v * w)), 3, 1);
    }
  }

  globalThis.quark_card = function (ctx, o) {
    var p = Q.byName[o.name];
    if (!p) { ctx.print(1, 0, String(o.name || ""), 1); return; }
    if (o.raw === null || o.raw === undefined) { ctx.print(1, 0, p.name, 1); ctx.print(1, 12, "--", 1); return; }
    var values = {};
    for (var k in (o.values || {})) values[k] = o.values[k];
    values[p.key] = o.raw;
    var key = p.key, W = o.w, H = o.h, top = 12, bh = H - top - 1;
    var label = p.name, text = Q.describe(key, values);
    header(ctx, o, label, text);

    switch (key) {
      case "tx_type": {
        var type = Q.val(values, "tx_type"), va = Q.val(values, "tx_var");
        var fam = Q.M.texTypes[Math.round(type)].family;
        ctx.print(1, top + 1, Q.familyName(type), 1);
        ctx.print(1, top + 11, Q.fit(ctx, key === "tx_type" ? Q.variationText(values) : Q.byKey.tx_type.opts[Math.round(type)], 46), 1);
        Q.plot(ctx, function (t) { return Q.texWave(type, va, t); }, 50, top, W - 52, bh - 4, fam === 0 ? 1 : 2);
        bar(ctx, 50, H - 5, W - 52, va, false);
        return;
      }
      case "ln_shape": {
        var sh = Q.val(values, "ln_shape"), lv = Q.val(values, "ln_var");
        if (Q.lineIsWave(sh)) {
          var to = Q.wenvTo(lv, Q.val(values, "ln_wenv"));
          ctx.print(1, top + 1, "WAVETABLE", 1);
          ctx.print(1, top + 11, Q.fit(ctx, "Pos " + Q.pct(lv), 46), 1);
          Q.plot(ctx, function (t) { return Q.lineWave(sh, lv, t); }, 50, top, W - 52, bh - 4, 2);
          /* the position bar, with the span the envelope sweeps */
          var bx = 50, bw = W - 52, y0 = H - 5;
          ctx.fillRect(bx, y0 + 2, bw, 1, 1);
          var a0 = Math.round(Math.min(lv, to) * (bw - 1)), a1 = Math.round(Math.max(lv, to) * (bw - 1));
          ctx.fillRect(bx + a0, y0 + 1, Math.max(1, a1 - a0 + 1), 3, 1);
          ctx.fillRect(bx + Math.round(lv * (bw - 1)), y0, 1, 5, 1);
          return;
        }
        Q.plot(ctx, function (t) { return Q.lineWave(sh, lv, t); }, 2, top, 44, bh, 2);
        var names = ["PWM", "TREM", "VIB"];
        for (var i = 0; i < 3; i++) {
          var f = Math.max(0, Math.min(1, lv * 3 - i));
          ctx.print(52, top + i * 9, names[i], 1);
          bar(ctx, 76, top + i * 9 + 1, W - 78, f, false);
        }
        return;
      }
      case "ens_amt": case "ens_type": {
        var et = Q.byKey.ens_type.opts[Math.round(Q.val(values, "ens_type"))];
        ctx.print(1, top + 1, et, 1);
        var amt = Q.val(values, "ens_amt"), now = (o.nowMs || 0) / 1000;
        for (var k = 0; k < 3; k++) {
          var xx = 56 + k * 16, wob = Math.sin(now * 3.6 + k * 2.094) * 5 * amt;
          ctx.fillRect(Math.round(xx + wob), top + 4 + k * 3, 3, 3, 1);
        }
        bar(ctx, 2, H - 5, W - 4, amt, false);
        return;
      }
      case "arp_rhy": case "arp_gate": case "arp_swing": {
        var rh = Q.M.arpRhythms[Math.round(Q.val(values, "arp_rhy"))];
        var ag = Q.val(values, "arp_gate"), gate = 0.05 + 1.95 * ag * ag, sw = 0.33 * Q.val(values, "arp_swing");
        var cw = (W - 4) / 16;
        for (var stp = 0; stp < 16; stp++) {
          var on = (rh[0] >> (stp % rh[1])) & 1;
          var x0 = 2 + stp * cw + (stp & 1 ? sw * cw : 0);
          if (on) ctx.fillRect(Math.round(x0), top + 3, Math.max(1, Math.round(cw * Math.min(1, gate) - 1)), bh - 6, 1);
          else ctx.fillRect(Math.round(x0), top + bh - 4, 1, 1, 1);
        }
        return;
      }
      case "sh_pitch": case "sh_amt": {
        var dec = Q.val(values, "rv_decay"), sa = Q.val(values, "sh_amt");
        var semis = Q.M.shimSemis[Math.round(Q.val(values, "sh_pitch"))];
        var pre = Q.revPre(Q.val(values, "rv_size"));
        for (var xi = 0; xi < W - 4; xi++) {
          var tt = xi / (W - 4) * 2.5, hgt = tt < pre ? 0 : Math.exp(-(tt - pre) * 3.5 / (0.35 + 3 * dec));
          var hh = Math.round(hgt * (bh - 2) * Math.max(0.15, Q.val(values, "rv_amt")));
          if (hh > 0 && xi % 2 === 0) ctx.fillRect(2 + xi, top + bh - hh, 1, hh, 1);
          var sy = Math.round(top + bh / 2 - (semis / 24) * (bh / 2) * Math.min(1, tt) * sa);
          if (sa > 0.01 && xi % 3 === 0) ctx.fillRect(2 + xi, sy, 1, 1, 1);
        }
        return;
      }
      case "tx_detune": case "tx_bloom": {
        var det = Q.val(values, "tx_detune"), bl = Q.val(values, "tx_bloom");
        ctx.print(1, top + 1, "CHORD", 1);
        ctx.print(1, top + 11, Q.fit(ctx, key === "tx_detune" ? "Bloom " + Q.describe("tx_bloom", values) : Q.describe("tx_detune", values), 46), 1);
        var gx = 50, gw = W - 52, cy = top + Math.floor(bh / 2), span = bh / 2 - 1;
        var tb = bl < 0.005 ? 0 : Math.round(gw * (0.08 + 0.85 * Math.sqrt(bl)));
        for (var n = 0; n < 5; n++) {
          var c = Q.detCents(n, det) / 45, py = cy;
          for (var gxi = 1; gxi < gw; gxi++) {
            var kk2 = tb <= 0 ? 1 : Math.min(1, gxi / tb), yy2 = Math.round(cy - c * kk2 * span);
            Q.line(ctx, gx + gxi - 1, py, gx + gxi, yy2);
            py = yy2;
          }
        }
        return;
      }
      case "arp_oct": case "arp_pat": case "arp_mode": {
        if (key === "arp_mode" && Math.round(Q.val(values, "arp_mode")) === 0) { ctx.print(1, top + 1, "Arp off: Mono", 1); ctx.print(1, top + 11, "plays its keys", 1); return; }
        /* the contour the type plays from a 3-note chord: one column per step */
        var pv = Q.arpPreview(values, 16), mn = 99, mx = -99;
        pv.forEach(function (st) { st.forEach(function (x) { mn = Math.min(mn, x); mx = Math.max(mx, x); }); });
        var span = Math.max(3, mx - mn + 1), cw = Math.floor((W - 4) / 16), ch2 = Math.max(1, Math.floor(bh / span));
        pv.forEach(function (st, i) {
          st.forEach(function (x) { ctx.fillRect(2 + i * cw, top + bh - (x - mn + 1) * ch2, Math.max(1, cw - 1), Math.max(1, ch2 - 1), 1); });
        });
        return;
      }
      case "arp_drop": case "arp_vrnd": case "arp_grnd": case "arp_ornd": case "arp_nrnd": case "arp_vel": case "arp_loop": {
        /* 16 steps of the variation, drawn as the dice would fall (illustrative) */
        var amt = key === "arp_vel" ? (Math.round(Q.val(values, "arp_vel")) || 100) / 127 : key === "arp_loop" ? 0.5 : Q.val(values, key);
        var lp = Math.round(Q.val(values, "arp_loop")), cw2 = Math.floor((W - 4) / 16);
        for (var i2 = 0; i2 < 16; i2++) {
          var st2 = lp > 0 ? i2 % lp : i2, r = ((st2 * 2654435761) >>> 0) % 1000 / 1000;
          var hgt = bh - 2;
          if (key === "arp_drop") { if (r < amt) { ctx.fillRect(2 + i2 * cw2, top + bh - 2, cw2 - 1, 1, 1); continue; } }
          else if (key === "arp_vel") hgt = Math.round((bh - 2) * amt);
          else hgt = Math.round((bh - 2) * (1 - amt * r));
          ctx.fillRect(2 + i2 * cw2, top + bh - hgt, Math.max(1, cw2 - 1), Math.max(1, hgt), 1);
          if (lp > 0 && i2 % lp === 0) ctx.fillRect(2 + i2 * cw2, top, 1, 2, 1);
        }
        return;
      }
      case "arp_src": {
        var src = Math.round(Q.val(values, "arp_src")), md = Math.round(Q.val(values, "arp_mode"));
        if (md === 0) { ctx.print(1, top + 1, "Arp off: Mono", 1); ctx.print(1, top + 11, "plays its keys", 1); return; }
        ctx.print(1, top + 1, src ? "Mono arpeggiates" : "Mono arpeggiates", 1);
        ctx.print(1, top + 11, src ? "the Pads chord" : "the keys sent", 1);
        ctx.print(1, top + 21, src ? "(+ Mono keys)" : "to Mono", 1);
        return;
      }
      case "split": {
        var sp = Q.val(values, "split");
        for (var kk = 24; kk <= 96; kk++) {
          var x2 = 2 + Math.round((kk - 24) / 72 * (W - 5));
          var blk = [1, 3, 6, 8, 10].indexOf(kk % 12) >= 0;
          ctx.fillRect(x2, top + (blk ? 2 : 6), 1, blk ? 4 : 8, kk < sp ? 1 : (kk % 2));
        }
        ctx.print(2, top + 16, "Mono", 1);
        ctx.print(W - 2 - Q.tw(ctx, "Pads"), top + 16, "Pads", 1);
        return;
      }
    }
    var pv = Q.val(values, key), bipolar = p.mn < 0;
    var norm = (pv - p.mn) / ((p.mx - p.mn) || 1);
    bar(ctx, 2, top + Math.round(bh / 2) - 2, W - 4, bipolar ? pv : norm, bipolar);
  };
})();
