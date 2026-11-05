/* Quark: parameter cards (Schwung `card_script`), raised while a knob is held or turning.
 *
 * One drawer serves every carded knob. The header line is the parameter and its value in real units
 * (seconds, Hz, semitones, register names); the body is a small picture of what the knob does.
 * Only knobs whose picture adds something the page does not already show get a card: the Atmos wave
 * and the Melody shape (on their Variation knobs too), the detune fan, the LFOs, the shimmer tail,
 * a picture per arp knob, the keyboard split, the PCM Layer (set, start, pitch / fine beating, mode
 * routing) and Era, and the two FX controls, whose meaning depends on the
 * effect (a one-line card naming it). Plain amounts and enums use the cell itself.
 * o.raw may be null: then only the name is drawn.
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
    if (key === "fx_ctrl" || key === "fx_tone") {
      /* one line: what this knob is for the selected effect, and its value */
      var ft = Math.round(Q.val(values, "fx_type"));
      label = Q.byKey.fx_type.opts[ft] + " " + Q.fxCtrlName(key, ft);
      text = text.replace(/^(Depth|Fdbk|Shape|Tone) /, "");
      ctx.print(1, Math.max(0, Math.floor((H - 7) / 2)), Q.fit(ctx, label, W - Q.tw(ctx, text) - 6), 1);
      ctx.print(W - 1 - Q.tw(ctx, text), Math.max(0, Math.floor((H - 7) / 2)), text, 1);
      return;
    }
    header(ctx, o, label, text);

    switch (key) {
      case "tx_type": case "tx_var": {
        var type = Q.val(values, "tx_type"), va = Q.val(values, "tx_var");
        var fam = Q.M.texTypes[Math.round(type)].family;
        ctx.print(1, top + 1, Q.familyName(type), 1);
        ctx.print(1, top + 11, Q.fit(ctx, key === "tx_type" ? Q.variationText(values) : Q.byKey.tx_type.opts[Math.round(type)], 46), 1);
        if (fam === 3) { ctx.print(50, top + 1, Q.plSet(values) ? "+ Layer only" : "silent: no", 1); if (!Q.plSet(values)) ctx.print(50, top + 11, "Layer set", 1); return; }
        Q.plot(ctx, function (t) { return Q.texWave(type, va, t); }, 50, top, W - 52, bh - 4, fam === 0 ? 1 : 2);
        bar(ctx, 50, H - 5, W - 52, va, false);
        return;
      }
      case "ln_shape": case "ln_var": {
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
      case "arp_rhy": {
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
        /* the reverb tail (bars) and the shimmer voice climbing out of it (dots) */
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
      case "arp_pat": case "arp_mode": {
        if (key === "arp_mode" && Math.round(Q.val(values, "arp_mode")) === 0) { ctx.print(1, top + 1, "Arp off: Melody", 1); ctx.print(1, top + 11, "plays its keys", 1); return; }
        /* the contour the type plays from a 3-note chord: one column per step */
        var pv = Q.arpPreview(values, 16), mn = 99, mx = -99;
        pv.forEach(function (st) { st.forEach(function (x) { mn = Math.min(mn, x); mx = Math.max(mx, x); }); });
        var span = Math.max(3, mx - mn + 1), cw = Math.floor((W - 4) / 16), ch2 = Math.max(1, Math.floor(bh / span));
        pv.forEach(function (st, i) {
          st.forEach(function (x) { ctx.fillRect(2 + i * cw, top + bh - (x - mn + 1) * ch2, Math.max(1, cw - 1), Math.max(1, ch2 - 1), 1); });
        });
        return;
      }
      case "arp_src": {
        /* where the arp's notes come from: the keys you play, or the chord Atmos holds */
        var src = Math.round(Q.val(values, "arp_src"));
        if (Math.round(Q.val(values, "arp_mode")) === 0) { ctx.print(1, top + 1, "Arp off: Melody", 1); ctx.print(1, top + 11, "plays its keys", 1); return; }
        var sx = 4, sy0 = top + 2;
        if (src) { for (var c3 = 0; c3 < 3; c3++) ctx.fillRect(sx, sy0 + 3 + c3 * 4, 18, 2, 1); }
        else { for (var kq = 0; kq < 4; kq++) { ctx.fillRect(sx + kq * 5, sy0, 1, 14, 1); if (kq < 3) ctx.fillRect(sx + kq * 5 + 3, sy0, 2, 8, 1); } ctx.fillRect(sx, sy0 + 14, 16, 1, 1); }
        ctx.print(sx, top + bh - 7, src ? "Atmos" : "Keys", 1);
        Q.line(ctx, 32, sy0 + 8, W - 40, sy0 + 8);
        Q.line(ctx, W - 44, sy0 + 5, W - 40, sy0 + 8); Q.line(ctx, W - 44, sy0 + 11, W - 40, sy0 + 8);
        for (var ns = 0; ns < 4; ns++) ctx.fillRect(W - 34 + ns * 8, sy0 + 12 - ns * 3, 5, 2, 1);
        ctx.print(W - 2 - Q.tw(ctx, "Melody"), top + bh - 7, "Melody", 1);
        return;
      }
      case "arp_oct": {
        /* an octave ladder (-3..+3, 0 marked): the octaves the arp climbs, in order, left to right */
        var seq = Q.M.arpOctSeq[Math.max(0, Math.min(Q.M.arpOctSeq.length - 1, Math.round(Q.val(values, "arp_oct"))))];
        var rh = Math.max(2, Math.floor(bh / 7)), ox = 14, ow = W - 16, cols = 12, cwo = Math.floor(ow / cols);
        for (var o = -3; o <= 3; o++) {
          var yo = top + (3 - o) * rh;
          for (var xo = ox; xo < ox + ow; xo += o === 0 ? 1 : 3) ctx.fillRect(xo, yo + rh - 1, 1, 1, 1);
        }
        ctx.print(1, top + 3 * rh - 2, "0", 1);
        for (var ci = 0; ci < cols; ci++) {
          var oc = seq[ci % seq.length];
          ctx.fillRect(ox + ci * cwo, top + (3 - oc) * rh, Math.max(1, cwo - 2), Math.max(1, rh - 1), 1);
        }
        return;
      }
      case "arp_gate": {
        /* 8 steps on a grid: each note held for the gate length (over 100% ties into the next) */
        var gt = 0.05 + 1.95 * Math.pow(Q.val(values, "arp_gate"), 2), sw8 = (W - 4) / 8;
        for (var g8 = 0; g8 <= 8; g8++) ctx.fillRect(Math.round(2 + g8 * sw8), top, 1, 3, 1);
        for (var n8 = 0; n8 < 8; n8++) {
          var gx0 = Math.round(2 + n8 * sw8), gl = Math.max(1, Math.round(sw8 * gt) - 1);
          ctx.fillRect(gx0, top + 6 + (n8 % 2) * 6, Math.min(gl, W - 2 - gx0), 4, 1);
        }
        return;
      }
      case "arp_swing": {
        /* straight 16ths (ticks) against the swung notes: every second note pushed late */
        var swv = 0.33 * Q.val(values, "arp_swing"), s16 = (W - 4) / 8;
        for (var t16 = 0; t16 < 8; t16++) {
          var tx0 = Math.round(2 + t16 * s16);
          for (var yy = top; yy < top + bh; yy += 3) ctx.fillRect(tx0, yy, 1, 1, 1);
          var nx = Math.round(tx0 + ((t16 & 1) ? swv * s16 * 2 : 0));
          ctx.fillRect(nx, top + 5, 4, 8, 1);
          if (t16 & 1 && swv > 0.01) Q.line(ctx, tx0, top + 16, nx, top + 16);
        }
        return;
      }
      case "arp_drop": {
        /* 16 steps: the ones Drop skips are crossed out */
        var amt = Q.val(values, "arp_drop"), cwd = Math.floor((W - 4) / 16);
        for (var d16 = 0; d16 < 16; d16++) {
          var dx0 = 2 + d16 * cwd, rr = ((d16 * 2654435761) >>> 0) % 1000 / 1000;
          if (rr < amt) { Q.line(ctx, dx0, top + 6, dx0 + cwd - 2, top + 14); Q.line(ctx, dx0, top + 14, dx0 + cwd - 2, top + 6); }
          else ctx.fillRect(dx0, top + 6, cwd - 1, 9, 1);
        }
        return;
      }
      case "arp_vel": case "arp_vrnd": {
        /* velocity per step: one level (Velocity), or as played; the random range drawn as a band */
        var fv = Math.round(Q.val(values, "arp_vel")), base = fv ? fv / 127 : 0.75, vr = Q.val(values, "arp_vrnd");
        var played = [0.8, 0.6, 0.9, 0.7, 0.75, 0.55, 0.85, 0.65], cwv = Math.floor((W - 4) / 16), hv = bh - 1;
        var lo = Math.max(0.05, base - 0.5 * vr), hi = Math.min(1, base + 0.5 * vr);
        if (vr > 0.01) for (var bx2 = 2; bx2 < W - 2; bx2 += 2) { ctx.fillRect(bx2, top + hv - Math.round(hi * hv), 1, 1, 1); ctx.fillRect(bx2, top + hv - Math.round(lo * hv), 1, 1, 1); }
        for (var v16 = 0; v16 < 16; v16++) {
          var rv = ((v16 * 2246822519 + 7) >>> 0) % 1000 / 1000 - 0.5;
          var lv2 = (fv ? base : played[v16 % 8]) + rv * vr;
          var hh2 = Math.max(1, Math.round(Math.max(0.05, Math.min(1, lv2)) * hv));
          ctx.fillRect(2 + v16 * cwv + 1, top + hv - hh2, Math.max(1, cwv - 3), hh2, 1);
        }
        return;
      }
      case "arp_grnd": {
        /* note lengths on a piano-roll lane: Gate Random stretches and shortens each one */
        var gr = Q.val(values, "arp_grnd"), gt2 = 0.05 + 1.95 * Math.pow(Q.val(values, "arp_gate"), 2), s8 = (W - 4) / 8;
        for (var r8 = 0; r8 < 8; r8++) {
          var rnd = ((r8 * 3266489917 + 3) >>> 0) % 1000 / 1000 - 0.5;
          var len = Math.max(1, Math.round(s8 * Math.max(0.05, gt2 * (1 + 1.6 * rnd * gr)) - 1));
          var rx = Math.round(2 + r8 * s8);
          ctx.fillRect(rx, top + 3 + (r8 % 3) * 6, Math.min(len, W - 2 - rx), 4, 1);
          ctx.fillRect(rx, top, 1, 2, 1);
        }
        return;
      }
      case "arp_ornd": {
        /* three octave lanes: Oct Random throws notes up or down an octave */
        var orn = Q.val(values, "arp_ornd"), cwo2 = Math.floor((W - 4) / 16), lh = Math.floor(bh / 3);
        for (var l3 = 0; l3 < 3; l3++) for (var xl = 2; xl < W - 2; xl += l3 === 1 ? 2 : 4) ctx.fillRect(xl, top + l3 * lh + lh - 1, 1, 1, 1);
        for (var o16 = 0; o16 < 16; o16++) {
          var ro = ((o16 * 2654435761 + 11) >>> 0) % 1000 / 1000, lane = ro < orn ? (o16 & 1 ? 0 : 2) : 1;
          ctx.fillRect(2 + o16 * cwo2, top + lane * lh + 1, Math.max(1, cwo2 - 1), Math.max(1, lh - 3), 1);
        }
        return;
      }
      case "arp_nrnd": {
        /* the arp's contour; Note Random swaps some steps for another chord note (outlined) */
        var nrn = Q.val(values, "arp_nrnd"), pvn = Q.arpPreview(values, 16);
        if (!pvn.length) { ctx.print(1, top + 1, "Arp off", 1); return; }
        var mnn = 99, mxn = -99;
        pvn.forEach(function (st) { st.forEach(function (x) { mnn = Math.min(mnn, x); mxn = Math.max(mxn, x); }); });
        var spn = Math.max(3, mxn - mnn + 1), cwn = Math.floor((W - 4) / 16), chn = Math.max(2, Math.floor(bh / spn));
        pvn.forEach(function (st, i) {
          var rn = ((i * 2246822519 + 5) >>> 0) % 1000 / 1000, x0n = 2 + i * cwn;
          st.forEach(function (x) {
            if (rn < nrn) {
              var alt = mnn + ((x - mnn + 1 + (i % (spn - 1))) % spn), yb = top + bh - (alt - mnn + 1) * chn;
              ctx.fillRect(x0n, yb, cwn - 1, 1, 1); ctx.fillRect(x0n, yb + chn - 2, cwn - 1, 1, 1);
              ctx.fillRect(x0n, yb, 1, chn - 1, 1); ctx.fillRect(x0n + cwn - 2, yb, 1, chn - 1, 1);
            } else ctx.fillRect(x0n, top + bh - (x - mnn + 1) * chn, Math.max(1, cwn - 1), Math.max(1, chn - 1), 1);
          });
        });
        return;
      }
      case "arp_loop": {
        /* the random variation repeating every Loop steps (brackets); Free never repeats */
        var lp = Math.round(Q.val(values, "arp_loop")), cwl = Math.floor((W - 4) / 16);
        for (var l16 = 0; l16 < 16; l16++) {
          var stp = lp > 0 ? l16 % lp : l16, rl = ((stp * 2654435761 + 1) >>> 0) % 1000 / 1000;
          var hl = Math.max(2, Math.round((bh - 6) * (0.3 + 0.7 * rl)));
          ctx.fillRect(2 + l16 * cwl, top + bh - hl, Math.max(1, cwl - 1), hl, 1);
          if (lp > 0 && stp === 0) { ctx.fillRect(2 + l16 * cwl, top, 1, 4, 1); ctx.fillRect(2 + l16 * cwl, top, Math.min(lp, 16 - l16) * cwl - 1, 1, 1); }
        }
        return;
      }
      case "pl_b": case "pl_blend": {
        /* Layer A (top) and B (bottom), named, each outline as tall as its share of the Blend */
        var la2 = Q.plSet(values, 0), lb2 = Q.plSet(values, 1), bl2 = Math.max(0, Math.min(1, Q.val(values, "pl_blend")));
        var rh3 = Math.floor(bh / 2), sides = [[la2, "A", Math.cos(bl2 * Math.PI / 2)], [lb2, "B", Math.sin(bl2 * Math.PI / 2)]];
        sides.forEach(function (sd, r) {
          var y0 = top + r * rh3;
          ctx.print(1, y0 + Math.floor((rh3 - 7) / 2), sd[1], 1);
          if (!sd[0]) { Q.dotted(ctx, 9, y0 + Math.floor(rh3 / 2), W - 10); return; }
          var g = !la2 || !lb2 ? 1 : sd[2];
          Q.pcmOutline(ctx, sd[0], 9, y0 + 1, W - 10, Math.max(1, Math.round((rh3 - 2) * Math.max(0.15, g))), 2);
        });
        return;
      }
      case "pl_wave": {
        /* the Layer set's outline, its loop bracket and where Start puts the note */
        var ps = Q.plSet(values, 0);
        if (!ps) { ctx.print(1, top + 1, "Layer A Off", 1); Q.dotted(ctx, 0, top + Math.floor(bh / 2), W); return; }
        var big = false, ox = big ? 1 : 46, ow = W - ox - 1;
        if (!big) {
          ctx.print(1, top + 1, ps.zones + (ps.zones === 1 ? " zone" : " zones"), 1);
          ctx.print(1, top + 11, ps.loop ? "loops" : "one-shot", 1);
        }
        Q.pcmOutline(ctx, ps, ox, top + 2, ow, bh - 6, 2);
        if (ps.loop) {
          var lx2 = ox + Math.round(ps.loop[0] * (ow - 1));
          ctx.fillRect(lx2, top, 1, 3, 1); ctx.fillRect(ox + ow - 1, top, 1, 3, 1); ctx.fillRect(lx2, top, ox + ow - lx2, 1, 1);
        }
        var sx2 = ox + Math.round(Q.plStartFrac(ps, Q.val(values, "pl_start")) * (ow - 1));
        Q.dotted(ctx, ox, H - 3, ow);
        ctx.fillRect(Math.max(0, sx2 - 1), big ? top : H - 4, big ? 3 : 3, big ? bh : 3, 1);
        return;
      }
      case "pl_tune": {
        /* the interval to the main voice, or (inside +-50 ct) how fast the two beat at A4 (one second drawn) */
        var tu2 = Q.plTune(Q.val(values, "pl_tune")), st2 = tu2.st, ct2 = tu2.ct;
        var IV = ["unison", "min 2nd", "maj 2nd", "min 3rd", "maj 3rd", "4th", "tritone", "5th", "min 6th", "maj 6th", "min 7th", "maj 7th"];
        var a12 = ((st2 % 12) + 12) % 12, oc = Math.floor(Math.abs(st2) / 12);
        var ivn = st2 === 0 ? "unison" : (st2 < 0 ? "-" : "+") + (a12 === 0 ? oc + " oct" : IV[st2 < 0 ? (12 - a12) % 12 : a12] + (oc ? "+" + oc + "oc" : ""));
        ctx.print(1, top + 1, Q.fit(ctx, ivn, 54), 1);
        var beat = ct2 === 0 || a12 !== 0 ? 0 : 440 * Math.abs(Math.pow(2, ct2 / 1200) - 1);
        ctx.print(1, top + 11, beat ? beat.toFixed(1) + " beat/s" : "no beat", 1);
        /* the beat envelope of the two summed partials over one second, mirrored */
        var bx2 = 58, bw2 = W - bx2 - 1, cy3 = top + Math.floor(bh / 2), am3 = Math.floor(bh / 2) - 1, py3 = null;
        Q.dotted(ctx, bx2, cy3, bw2);
        for (var xb = 0; xb < bw2; xb++) {
          var env = beat ? Math.abs(Math.cos(Math.PI * Math.min(beat, 8) * xb / bw2)) : 1;
          var yb = Math.round(env * am3);
          if (py3 !== null) { Q.line(ctx, bx2 + xb - 1, cy3 - py3, bx2 + xb, cy3 - yb); Q.line(ctx, bx2 + xb - 1, cy3 + py3, bx2 + xb, cy3 + yb); }
          py3 = yb;
        }
        return;
      }
      case "pl_mode": {
        var md = Math.max(0, Math.min(3, Math.round(Q.val(values, "pl_mode"))));
        ctx.print(1, top + 2, Q.plModeText[md], 1);
        /* the voice path, with the Layer joining before or after the filter */
        var path = ["OSC", "FLT", "AMP"], px4 = 2, y4 = top + 14;
        for (var pi = 0; pi < 3; pi++) {
          ctx.print(px4, y4, path[pi], 1);
          var w4 = Q.tw(ctx, path[pi]);
          if (pi < 2) Q.dotted(ctx, px4 + w4 + 2, y4 + 3, 10);
          if ((md < 2 && pi === 0) || (md >= 2 && pi === 1)) {
            var jx = px4 + w4 + 7;
            ctx.fillRect(jx - 1, y4 + 9, 3, 1, 1);   /* the Layer joins here, from below */
            ctx.fillRect(jx, y4 + 9, 1, Math.max(1, H - y4 - 10), 1);
          }
          px4 += w4 + 14;
        }
        return;
      }
      case "pl_era": {
        var er = Math.max(0, Math.min(2, Math.round(Q.val(values, "pl_era"))));
        ctx.print(1, top + 1, Q.fit(ctx, Q.eraText[er], W - 2), 1);
        ctx.print(1, top + 11, ["all zones", "every 2nd zone", "every 3rd zone"][er], 1);
        Q.plot(ctx, function (t) { return Q.eraWave(er, t); }, 76, top + 9, W - 77, bh - 9, 1);
        return;
      }
      case "an_shape":
      case "an_dest":
      case "an_lfo2":
      case "an_mode": {
        /* two cycles of the selected shape: the main LFO solid, LFO 2 dotted (pair targets only),
         * each labelled with the destination it moves */
        var shp = Math.round(Q.val(values, "an_shape")), r2 = Q.val(values, "an_lfo2");
        var src = Q.M.anSrc[Math.max(0, Math.min(Q.M.anSrc.length - 1, Math.round(Q.val(values, "an_dest"))))];
        var t1 = src.indexOf(1), t2 = src.indexOf(2);
        var gx2 = 26, gw2 = W - 28, cy2 = top + Math.floor(bh / 2), am2 = bh / 2 - 1, py1 = 0;
        var lm = Math.round(Q.val(values, "an_mode"));
        if (lm > 0) {
          /* per voice: two more voices drawn sparse. Spread: the same LFO at scattered phases;
           * Retrig: each voice's cycle starts at its note (marked) */
          for (var vv = 1; vv <= 2; vv++) {
            var x0v = lm === 2 ? Math.round(gw2 * vv / 3) : 0, offv = lm === 2 ? 0 : (vv * 0.618034) % 1;
            if (lm === 2) ctx.fillRect(gx2 + x0v, top, 1, 3, 1);
            for (var xv = x0v; xv < gw2; xv += 3)
              ctx.fillRect(gx2 + xv, Math.round(cy2 - am2 * Q.lfoShape(shp, 2 * (xv - x0v) / gw2 + offv, 5 + vv)), 1, 1, 1);
          }
        }
        ctx.print(1, top + 1, Q.M.anTargets[t1], 1);
        if (t2 >= 0) ctx.print(1, top + bh - 8, Q.M.anTargets[t2], 1);
        for (var xi2 = 0; xi2 < gw2; xi2++) {
          var ph = 2 * xi2 / gw2;
          var y1 = Math.round(cy2 - am2 * Q.lfoShape(shp, ph));
          if (xi2) Q.line(ctx, gx2 + xi2 - 1, py1, gx2 + xi2, y1);
          if (t2 >= 0 && xi2 % 2 === 0) ctx.fillRect(gx2 + xi2, Math.round(cy2 - am2 * Q.lfoShape(shp, ph * r2, 3)), 1, 1, 1);
          py1 = y1;
        }
        if (t2 < 0 && key === "an_lfo2") ctx.print(1, top + bh - 8, "Pair targets", 1);
        else if (t2 < 0 && lm > 0) ctx.print(1, top + bh - 8, "Voice", 1);
        return;
      }
      case "split":
      case "key_mode": {
        /* the keyboard C1..C7 (C3 in the middle), with the Melody keys filled; the split mark sits
         * between the two layers */
        var sp = Math.round(Q.val(values, "split")), km = Math.round(Q.val(values, "key_mode"));
        var kx = function (n) { return 2 + Math.round((n - 24) / 72 * (W - 5)); };
        for (var kk = 24; kk <= 96; kk++) {
          var blk = [1, 3, 6, 8, 10].indexOf(kk % 12) >= 0;
          var mono = km === 4 || (km === 1 && kk < sp) || (km === 2 && kk >= sp);
          ctx.fillRect(kx(kk), top + (blk ? 2 : 6), 1, blk ? 4 : 8, mono ? 1 : (kk % 2));
        }
        ctx.fillRect(kx(60), top, 1, 1, 1);
        var ly = top + 16;
        if (km === 1 || km === 2) {
          ctx.fillRect(kx(sp) - 1, top - 1, 1, 16, 1);
          ctx.print(2, ly, km === 1 ? "Melody" : "Atmos", 1);
          ctx.print(W - 2 - Q.tw(ctx, km === 1 ? "Atmos" : "Melody"), ly, km === 1 ? "Atmos" : "Melody", 1);
        } else {
          var all = km === 0 ? "Atmos + Melody, all keys" : km === 3 ? "Atmos, all keys" : "Melody, all keys";
          if (Q.tw(ctx, all) > W - 4) all = km === 0 ? "Atmos + Melody" : km === 3 ? "Atmos only" : "Melody only";
          ctx.print(2, ly, Q.fit(ctx, all, W - 4), 1);
        }
        return;
      }
    }
    var pv = Q.val(values, key), bipolar = p.mn < 0;
    var norm = (pv - p.mn) / ((p.mx - p.mn) || 1);
    bar(ctx, 2, top + Math.round(bh / 2) - 2, W - 4, bipolar ? pv : norm, bipolar);
  };
})();
