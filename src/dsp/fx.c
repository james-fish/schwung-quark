/* Effects.
 *
 *  FX 1   one multi-effect at a time on the FX bus: Chorus x2, Flanger x2, Phaser x2, Tremolo, Drive,
 *         Delay, Ping Pong, Tape Echo. Control is the main knob (rate / drive / time), Amount the mix,
 *         Control 2 the second layer (depth / feedback / tone).
 *  FX 2   shimmer reverb: an 8-line FDN (Hadamard feedback, damped, slowly modulated - the structure
 *         Amino and Crest use, MIT) with a two-grain pitch shifter reading the tank output and
 *         feeding it back in, so the tail climbs (or falls) by the Shimmer Pitch interval.
 */
#include "quark.h"

#include <math.h>
#include <string.h>

#include "tables_gen.h"

static const int REV_BASE[8] = {1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116};
static const int DIFF_LEN[2][4] = {{142, 107, 379, 277}, {151, 113, 389, 293}};

static void dl_setup(QDelay *d, float *buf, int n) {
    d->buf = buf;
    d->size = n;
    d->w = 0;
}

static inline void dl_write(QDelay *d, float x) {
    d->buf[d->w] = x;
    if (++d->w >= d->size) d->w = 0;
}

static inline float dl_read(const QDelay *d, float delay) {
    float pos = (float)d->w - 1.0f - delay;
    while (pos < 0.0f) pos += (float)d->size;
    int i = (int)pos;
    float f = pos - (float)i;
    if (i >= d->size) i -= d->size;
    int i1 = i + 1 < d->size ? i + 1 : 0;
    return d->buf[i] + (d->buf[i1] - d->buf[i]) * f;
}

/* power-of-two buffers: mask instead of wrap loops (reverb lines and pre-delay) */
static inline float dl_read_m(const QDelay *d, float delay) {
    float pos = (float)d->w - 1.0f - delay + (float)d->size;
    int i = (int)pos;
    float f = pos - (float)i;
    int mask = d->size - 1;
    float a = d->buf[i & mask], b = d->buf[(i + 1) & mask];
    return a + (b - a) * f;
}

static inline float dl_tap(const QDelay *d, int delay) {
    int i = d->w - 1 - delay;
    while (i < 0) i += d->size;
    return d->buf[i];
}

void q_fx_init(QInstance *q) {
    QFx *f = &q->fx;
    for (int c = 0; c < 2; c++) {
        dl_setup(&f->md[c], f->mod_buf[c], Q_MODDLY_N);
        dl_setup(&f->dd[c], f->dly_buf[c], Q_DLY_N);
        dl_setup(&f->mdl[c], f->mdl_buf[c], Q_DLY_N);
        dl_setup(&f->rv_pre[c], f->rv_pre_buf[c], Q_REV_PRE_N);
        for (int k = 0; k < 4; k++) dl_setup(&f->rv_diff[c][k], f->rv_diff_buf[c][k], DIFF_LEN[c][k] + 1);
    }
    for (int k = 0; k < 8; k++) {
        dl_setup(&f->rv_line[k], f->rv_line_buf[k], Q_REV_LINE_N);
        f->rv_mph[k] = (float)k * 0.137f;
    }
    f->fx_last_type = -1;
}

void q_fx_clear(QInstance *q) {
    QFx *f = &q->fx;
    memset(f->ens_buf, 0, sizeof f->ens_buf);
    memset(f->mod_buf, 0, sizeof f->mod_buf);
    memset(f->dly_buf, 0, sizeof f->dly_buf);
    memset(f->mdl_buf, 0, sizeof f->mdl_buf);
    f->mdl_live = 0;
    memset(f->rv_line_buf, 0, sizeof f->rv_line_buf);
    memset(f->rv_diff_buf, 0, sizeof f->rv_diff_buf);
    memset(f->rv_pre_buf, 0, sizeof f->rv_pre_buf);
    memset(f->sh_buf, 0, sizeof f->sh_buf);
    memset(f->ap, 0, sizeof f->ap);
    memset(f->rv_damp, 0, sizeof f->rv_damp);
    f->ap_fb[0] = f->ap_fb[1] = 0.0f;
    f->sh_out = f->sh_hp = f->sh_lp = 0.0f;
    f->rv_tail = 0;
    f->fx_tail = 0;
}

/* ------------------------------------------------------------------ FX 1 */

void q_fx1(QInstance *q, float *l, float *r, int n) {
    const float *P = q->ps;
    QFx *f = &q->fx;
    float sr = q->sr;
    float amt = P[P_FX_AMT];
    int type = q_clampi((int)(P[P_FX_TYPE] + 0.5f), 0, 10);
    if (amt < 0.0005f && q->pv[P_FX_AMT] < 0.0005f) {
        f->fx_last_type = -1;
        return;
    }
    if (type != f->fx_last_type) {
        memset(f->mod_buf, 0, sizeof f->mod_buf);
        if (type >= 8 && (f->fx_last_type < 8)) memset(f->dly_buf, 0, sizeof f->dly_buf);
        memset(f->ap, 0, sizeof f->ap);
        f->ap_fb[0] = f->ap_fb[1] = 0.0f;
        f->dly_t = -1.0f;
        f->fx_last_type = type;
    }
    float c = P[P_FX_CTRL], t2 = P[P_FX_TONE];
    float ms = sr * 0.001f;

    if (type <= 3) { /* chorus / flanger */
        int flanger = type >= 2;
        float rate = flanger ? 0.03f * powf(100.0f, c) : 0.08f * powf(60.0f, c);
        float base, depth, fb = 0.0f;
        int voices = type == 1 ? 3 : 2;
        if (flanger) {
            base = (type == 3 ? 0.35f : 0.8f) * ms;
            depth = (type == 3 ? 2.5f : 3.5f) * ms;
            fb = (type == 3 ? -0.88f : 0.88f) * t2;
        } else {
            base = (type == 1 ? 14.0f : 7.5f) * ms;
            depth = (0.6f + 4.5f * t2) * ms;
        }
        float dry = flanger ? 1.0f - 0.5f * amt : 1.0f - 0.35f * amt;
        float wet = flanger ? 0.7f * amt : 0.85f * amt;
        float inc = rate / sr;
        for (int i = 0; i < n; i++) {
            f->fx_ph += inc;
            if (f->fx_ph >= 1.0f) f->fx_ph -= 1.0f;
            float out[2];
            for (int ch = 0; ch < 2; ch++) {
                float acc = 0.0f;
                for (int k = 0; k < voices; k++) {
                    float ph = f->fx_ph + (float)k / (float)voices + (ch ? 0.25f : 0.0f);
                    ph -= floorf(ph);
                    float lfo = flanger ? (ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph) : sinf(Q_TAU * ph);
                    float d = base + depth * (0.5f + 0.5f * lfo);
                    acc += dl_read(&f->md[ch], d);
                }
                out[ch] = acc / (float)voices;
            }
            dl_write(&f->md[0], l[i] + fb * out[0]);
            dl_write(&f->md[1], r[i] + fb * out[1]);
            l[i] = l[i] * dry + out[0] * wet;
            r[i] = r[i] * dry + out[1] * wet;
        }
    } else if (type <= 5) { /* phaser */
        int stages = type == 5 ? 8 : 4;
        float rate = 0.03f * powf(150.0f, c);
        float fb = (type == 5 ? 0.6f : 0.75f) * t2;
        for (int i0 = 0; i0 < n; i0 += Q_CTRL) {
            int m = n - i0 < Q_CTRL ? n - i0 : Q_CTRL;
            f->fx_ph += rate * (float)m / sr;
            f->fx_ph -= floorf(f->fx_ph);
            float a[2];
            for (int ch = 0; ch < 2; ch++) {
                float ph = f->fx_ph + (ch ? 0.18f : 0.0f);
                float sweep = 0.5f + 0.5f * sinf(Q_TAU * ph);
                float fc = 160.0f * powf(24.0f, sweep);
                float tw = tanf(Q_PI * fc / sr);
                a[ch] = (tw - 1.0f) / (tw + 1.0f);
            }
            for (int i = i0; i < i0 + m; i++) {
                for (int ch = 0; ch < 2; ch++) {
                    float *x = ch ? r : l;
                    float y = x[i] + fb * f->ap_fb[ch];
                    for (int s = 0; s < stages; s++) {
                        float o = a[ch] * y + f->ap[ch][s];
                        f->ap[ch][s] = y - a[ch] * o;
                        y = o;
                    }
                    f->ap_fb[ch] = y;
                    x[i] = x[i] * (1.0f - 0.5f * amt) + y * 0.5f * amt * 1.2f;
                }
            }
        }
    } else if (type == 6) { /* stereo tremolo */
        float rate = 0.5f * powf(28.0f, c);
        float sharp = 1.0f + 9.0f * t2;
        float norm = 1.0f / q_tanh(sharp);
        for (int i = 0; i < n; i++) {
            f->fx_ph += rate / sr;
            if (f->fx_ph >= 1.0f) f->fx_ph -= 1.0f;
            float s = q_tanh(sharp * sinf(Q_TAU * f->fx_ph)) * norm;
            l[i] *= 1.0f - amt * (0.5f + 0.5f * s);
            r[i] *= 1.0f - amt * (0.5f - 0.5f * s);
        }
    } else if (type == 7) { /* drive */
        float g = 1.0f + 30.0f * c * c;
        float lpc = 1.0f - expf(-Q_TAU * (700.0f * powf(17.0f, t2)) / sr);
        float hpc = 1.0f - expf(-Q_TAU * 40.0f / sr);
        float make = 1.0f / (0.6f + 0.4f * sqrtf(g));
        const float bias = 0.12f, tb = 0.11942f; /* tanh(0.12) */
        for (int i = 0; i < n; i++) {
            for (int ch = 0; ch < 2; ch++) {
                float *x = ch ? r : l;
                float in = x[i];
                f->drv_hp[ch] += (in - f->drv_hp[ch]) * hpc;
                float y = tanhf(g * (in - f->drv_hp[ch]) + bias) - tb;
                f->drv_lp[ch] += (y - f->drv_lp[ch]) * lpc;
                x[i] = in * (1.0f - amt) + f->drv_lp[ch] * make * amt;
            }
        }
    } else { /* delays */
        int di = q_clampi((int)(c * 11.99f), 0, 11);
        float bpm = q->bpm > 20.0f ? q->bpm : 120.0f;
        float target = Q_DELAY_BEATS[di] * 60.0f / bpm * sr;
        target = q_clamp(target, 10.0f, (float)(Q_DLY_N - 4096));
        if (f->dly_t < 0.0f) f->dly_t = target;
        float fb = 0.92f * t2;
        int tape = type == 10, ping = type == 9;
        float lpc = 1.0f - expf(-Q_TAU * (tape ? 3200.0f : 7500.0f) / sr);
        for (int i = 0; i < n; i++) {
            f->dly_t += (target - f->dly_t) * 0.0004f;
            float d = f->dly_t;
            float dl = d, dr = d;
            if (tape) {
                f->wow_ph += 0.6f / sr;
                if (f->wow_ph >= 1.0f) f->wow_ph -= 1.0f;
                float wob = 1.2f * ms * sinf(Q_TAU * f->wow_ph) + 0.15f * ms * sinf(Q_TAU * 7.3f * f->wow_ph);
                dl += wob;
                dr += wob * 0.8f;
            } else if (!ping) {
                dr = d * 1.0f;
            }
            float yl = dl_read(&f->dd[0], dl), yr = dl_read(&f->dd[1], dr);
            f->dly_lp[0] += (yl - f->dly_lp[0]) * lpc;
            f->dly_lp[1] += (yr - f->dly_lp[1]) * lpc;
            float fl = f->dly_lp[0], fr = f->dly_lp[1];
            if (tape) { fl = q_tanh(fl * 1.3f) * 0.8f; fr = q_tanh(fr * 1.3f) * 0.8f; }
            if (ping) {
                dl_write(&f->dd[0], 0.5f * (l[i] + r[i]) + fb * fr);
                dl_write(&f->dd[1], fb * fl);
            } else {
                dl_write(&f->dd[0], l[i] + fb * fl);
                dl_write(&f->dd[1], r[i] + fb * fr);
            }
            l[i] += yl * amt * 0.8f;
            r[i] += yr * amt * 0.8f;
        }
    }
}

/* ------------------------------------------------------------------ Mono delay
 * One knob: Amount raises the echo level and the feedback together. Time is a tempo-synced
 * division. The echoes run through a 150 Hz high-pass and a 5 kHz low-pass (in the loop too, so
 * repeats thin out and darken). Left and right keep their own lines, so Mono Spread survives. */
void q_mono_delay(QInstance *q, float *l, float *r, int n) {
    const float *P = q->ps;
    QFx *f = &q->fx;
    float amt = P[P_LN_DLY];
    if (amt < 0.003f) {
        if (f->mdl_live) {
            memset(f->mdl_buf, 0, sizeof f->mdl_buf);
            f->mdl_hp[0] = f->mdl_hp[1] = f->mdl_lp[0] = f->mdl_lp[1] = 0.0f;
            f->mdl_live = 0;
        }
        return;
    }
    float sr = q->sr;
    int di = q_clampi((int)(P[P_LN_DTIME] + 0.5f), 0, 11);
    float bpm = q->bpm > 20.0f ? q->bpm : 120.0f;
    float target = q_clamp(Q_DELAY_BEATS[di] * 60.0f / bpm * sr, 10.0f, (float)(Q_DLY_N - 4096));
    if (!f->mdl_live) { f->mdl_t = target; f->mdl_live = 1; }
    float wet = amt < 0.5f ? 1.4f * amt : 0.7f;
    float fb = 0.15f + 0.65f * amt;
    float hpc = 1.0f - expf(-Q_TAU * 150.0f / sr);
    float lpc = 1.0f - expf(-Q_TAU * 5000.0f / sr);
    for (int i = 0; i < n; i++) {
        f->mdl_t += (target - f->mdl_t) * 0.0004f;
        for (int c = 0; c < 2; c++) {
            float *x = c ? r : l;
            float y = dl_read(&f->mdl[c], f->mdl_t);
            f->mdl_lp[c] += (y - f->mdl_lp[c]) * lpc;
            f->mdl_hp[c] += (f->mdl_lp[c] - f->mdl_hp[c]) * hpc;
            float e = f->mdl_lp[c] - f->mdl_hp[c];
            dl_write(&f->mdl[c], x[i] + fb * e);
            x[i] += wet * e;
        }
    }
}

/* ------------------------------------------------------------------ shimmer reverb */

void q_reverb(QInstance *q, const float *il, const float *ir, float *ol, float *orr, int n) {
    const float *P = q->ps;
    QFx *f = &q->fx;
    float sr = q->sr;
    float sh_amt = P[P_SH_AMT];
    float ret = fmaxf(P[P_RV_AMT], 0.5f * sh_amt);
    float ret_t = fmaxf(q->pv[P_RV_AMT], 0.5f * q->pv[P_SH_AMT]);
    if (ret < 0.0005f && ret_t < 0.0005f) {
        if (f->rv_tail == 0) {
            memset(f->rv_line_buf, 0, sizeof f->rv_line_buf);
            memset(f->sh_buf, 0, sizeof f->sh_buf);
            memset(f->rv_damp, 0, sizeof f->rv_damp);
            f->rv_tail = -1;
        }
        return;
    }
    f->rv_tail = 0;

    float t60 = 0.35f * powf(80.0f, P[P_RV_DECAY]);
    float size = 0.55f + 1.6f * P[P_RV_SIZE];
    float dampc = 1.0f - expf(-Q_TAU * (1200.0f * powf(13.0f, P[P_RV_TONE])) / sr);
    float hpc = 1.0f - expf(-Q_TAU * 120.0f / sr);
    float pre = (0.005f + 0.07f * P[P_RV_SIZE]) * sr; /* pre-delay grows with the room */
    float len[8], gain[8], inc[8];
    for (int k = 0; k < 8; k++) {
        len[k] = (float)REV_BASE[k] * size;
        gain[k] = P[P_RV_DECAY] > 0.995f ? 0.9995f : powf(10.0f, -3.0f * len[k] / (t60 * sr));
        inc[k] = (0.5f + 0.13f * (float)k) / sr;
    }
    int si = q_clampi((int)(P[P_SH_PITCH] + 0.5f), 0, 7);
    float ratio = exp2f(Q_SHIM_SEMIS[si] / 12.0f);
    float W = 2048.0f;
    float sh_step = (1.0f - ratio) / W;
    float sh_fb = sh_amt * 0.55f;
    float sh_hpc = 1.0f - expf(-Q_TAU * 180.0f / sr);
    float sh_lpc = 1.0f - expf(-Q_TAU * 9000.0f / sr);
    const float dif = 0.62f, modd = 7.0f;

    for (int i = 0; i < n; i++) {
        float xs[2];
        for (int c = 0; c < 2; c++) {
            dl_write(&f->rv_pre[c], c ? ir[i] : il[i]);
            float x = pre > 1.0f ? dl_read_m(&f->rv_pre[c], pre) : (c ? ir[i] : il[i]);
            f->rv_hp[c] += (x - f->rv_hp[c]) * hpc;
            x -= f->rv_hp[c];
            x += f->sh_out * sh_fb;
            for (int k = 0; k < 4; k++) {
                float y = dl_tap(&f->rv_diff[c][k], DIFF_LEN[c][k] - 1);
                float w = x + dif * y;
                dl_write(&f->rv_diff[c][k], w);
                x = y - dif * w;
            }
            xs[c] = x;
        }
        float o[8];
        for (int k = 0; k < 8; k++) {
            f->rv_mph[k] += inc[k];
            if (f->rv_mph[k] >= 1.0f) f->rv_mph[k] -= 1.0f;
            float ph = f->rv_mph[k];
            float tri = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
            float y = dl_read_m(&f->rv_line[k], len[k] - 1.0f + modd * tri);
            f->rv_damp[k] += (y - f->rv_damp[k]) * dampc;
            o[k] = f->rv_damp[k];
        }
        float a = o[0] + o[1], b = o[0] - o[1], c2 = o[2] + o[3], d = o[2] - o[3];
        float e = o[4] + o[5], g = o[4] - o[5], h = o[6] + o[7], qq = o[6] - o[7];
        float a2 = a + c2, b2 = b + d, c3 = a - c2, d2 = b - d;
        float e2 = e + h, f2 = g + qq, g2 = e - h, h2 = g - qq;
        float mx[8] = {a2 + e2, b2 + f2, c3 + g2, d2 + h2, a2 - e2, b2 - f2, c3 - g2, d2 - h2};
        for (int k = 0; k < 8; k++)
            dl_write(&f->rv_line[k], mx[k] * 0.35355339f * gain[k] + xs[k & 1]);
        float outl = (o[0] + o[2] + o[4] + o[6]) * 0.32f;
        float outr = (o[1] + o[3] + o[5] + o[7]) * 0.32f;

        /* shimmer: two Hann-ish grains reading a short buffer of the tank output */
        if (sh_amt > 0.0005f) {
            float in = 0.5f * (outl + outr);
            f->sh_buf[f->sh_w] = in;
            f->sh_ph += sh_step;
            if (f->sh_ph >= 1.0f) f->sh_ph -= 1.0f;
            if (f->sh_ph < 0.0f) f->sh_ph += 1.0f;
            float p1 = f->sh_ph, p2 = p1 + 0.5f;
            if (p2 >= 1.0f) p2 -= 1.0f;
            float w1 = p1 * (1.0f - p1), w2 = p2 * (1.0f - p2);
            float gsum = 1.0f / (w1 + w2);
            float y = 0.0f;
            for (int gi = 0; gi < 2; gi++) {
                float dd = 2.0f + (gi ? p2 : p1) * W;
                float pos = (float)f->sh_w - dd;
                if (pos < 0.0f) pos += (float)Q_SHIM_N;
                int i0 = (int)pos;
                float fr = pos - (float)i0;
                float s0 = f->sh_buf[i0 & (Q_SHIM_N - 1)], s1 = f->sh_buf[(i0 + 1) & (Q_SHIM_N - 1)];
                y += (s0 + (s1 - s0) * fr) * (gi ? w2 : w1);
            }
            y *= gsum;
            f->sh_w = (f->sh_w + 1) & (Q_SHIM_N - 1);
            f->sh_hp += (y - f->sh_hp) * sh_hpc;
            f->sh_lp += ((y - f->sh_hp) - f->sh_lp) * sh_lpc;
            f->sh_out = f->sh_lp;
            outl += f->sh_out * sh_amt * 0.35f;
            outr += f->sh_out * sh_amt * 0.35f;
        } else {
            f->sh_out = 0.0f;
        }
        ol[i] += outl * ret;
        orr[i] += outr * ret;
    }
}
