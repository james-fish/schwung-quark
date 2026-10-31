/* Pads layer.
 *
 *  Strings family  A divide-down string machine. Each voice runs ONE phase accumulator at 16'; the
 *                  8', 4' and 2' registers are its phase doubled, so every register is phase-locked
 *                  the way a top-octave divider is. Seven PolyBLEP taps (3 saw, 4 square) are mixed by
 *                  the register points in tables_gen.h, then a one-pole "tone" per voice, then (on the
 *                  bus) a two-formant vowel bank for the choir registers. Variation sweeps the points.
 *  Wavetable       8 keyframes per table, band-limited into 8 mip levels at load (no aliasing above the
 *                  played note's Nyquist), linear interpolation in the frame and between frames, and an
 *                  optional detuned unison partner (Spread).
 *  FM              carrier + mod 1 (Variation = index, with feedback) + mod 2 (a decaying "tine").
 *
 * Every voice then goes through an optional TPT state-variable filter (LP12/LP24/BP/HP) and the amp
 * ADSR. Animate is one global LFO; Depth routes the per-voice Mod Env to Variation.
 */
#include "quark.h"

#include <math.h>
#include <string.h>

#include "tables_gen.h"

/* ------------------------------------------------------------------ wavetables */

#define Q_MIPS 8
static const int MIP_N[Q_MIPS] = {2048, 1024, 512, 256, 128, 64, 64, 64};
static const int MIP_H[Q_MIPS] = {256, 128, 64, 32, 16, 8, 4, 2};
#define Q_FRAME_FLOATS (2049 + 1025 + 513 + 257 + 129 + 65 + 65 + 65)

static float g_wt[Q_WT_COUNT][Q_WT_FRAMES][Q_FRAME_FLOATS];
static int g_mip_off[Q_MIPS];
static float g_sin[4097];
static volatile int g_tables_ready;
static const int AM_N[Q_AM_MIPS] = {256, 128, 64, 32, 32, 32, 32};
static const int AM_H[Q_AM_MIPS] = {64, 32, 16, 8, 4, 2, 1};
static int g_am_off[Q_AM_MIPS];
static float g_c128[128], g_s128[128];
/* chord stacks: the table cycle holds DIV note periods, so they get 2x the length and a harmonic budget
 * of DIV x the usual, capped at a quarter of each mip's length (4+ samples per cycle at the top) */
static const int CH_N[Q_MIPS] = {4096, 2048, 1024, 512, 256, 128, 64, 64};
#define Q_CH_FF (4097 + 2049 + 1025 + 513 + 257 + 129 + 65 + 65)
static float g_ch[Q_CH_COUNT][Q_WT_FRAMES][Q_CH_FF];
static int g_ch_off[Q_MIPS], g_ch_harm[Q_CH_COUNT][Q_MIPS];

/* in-place inverse complex FFT, n a power of two <= 4096; twiddles from g_sin */
static void ifft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = 4096 / len, half = len >> 1;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; k++) {
                float wr = g_sin[(k * step + 1024) & 4095], wi = g_sin[k * step];   /* e^{+j 2pi k/len} */
                float *ar = re + i + k, *ai = im + i + k, *br = re + i + k + half, *bi = im + i + k + half;
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr; *ai += ti;
            }
    }
}

static void ch_build(int t) {
    static float S[1025], C[1025], xr[4096], xi[4096];
    for (int m = 0; m < Q_MIPS; m++) {
        int hm = MIP_H[m] * Q_CH_DIV[t], cap = CH_N[m] / 4;
        g_ch_harm[t][m] = hm < cap ? hm : cap;
    }
    int hmax = g_ch_harm[t][0];
    for (int k = 0; k < Q_WT_FRAMES; k++) {
        memset(S, 0, sizeof S);
        memset(C, 0, sizeof C);
        /* each tone a saw (or odd-harmonic pulse) rolled off by tilt, shifted in time by a fixed
         * golden-ratio phase so the tones' edges do not all land together */
        for (int j = 0; j < Q_CH_TONES; j++) {
            int th = Q_CH_H[t][k][j];
            if (!th) continue;
            float w = Q_CH_W[t][k][j], phi = Q_TAU * (0.618034f * (float)j - floorf(0.618034f * (float)j));
            for (int n = 1; n * th <= hmax; n++) {
                if (Q_CH_ODD[t] && !(n & 1)) continue;
                float a = w / powf((float)n, Q_CH_TILT[t]);
                S[n * th] += a * cosf((float)n * phi);
                C[n * th] += a * sinf((float)n * phi);
            }
        }
        /* each mip: x[i] = sum S[h] sin(2pi h i/N) + C[h] cos(..), top quarter tapered, by inverse FFT */
        float scale = 1.0f;
        for (int m = 0; m < Q_MIPS; m++) {
            int N = CH_N[m], H = g_ch_harm[t][m];
            memset(xr, 0, sizeof(float) * (size_t)N);
            memset(xi, 0, sizeof(float) * (size_t)N);
            for (int h = 1; h <= H; h++) {
                float hf = (float)h / (float)H, tp = hf > 0.75f ? 1.0f - (hf - 0.75f) * 3.2f : 1.0f;
                xr[h] = 0.5f * tp * C[h]; xi[h] = -0.5f * tp * S[h];
                xr[N - h] = xr[h]; xi[N - h] = -xi[h];
            }
            ifft(xr, xi, N);
            float *dst = g_ch[t][k] + g_ch_off[m];
            if (m == 0) {
                float pk = 0.0f;
                for (int i = 0; i < N; i++) pk = fmaxf(pk, fabsf(xr[i]));
                scale = pk > 1e-6f ? 0.95f / pk : 1.0f;
            }
            for (int i = 0; i < N; i++) dst[i] = xr[i] * scale;
            dst[N] = dst[0];
        }
    }
}

void q_tables_init(void) {
    if (g_tables_ready) return;
    for (int i = 0; i <= 4096; i++) g_sin[i] = sinf(Q_TAU * (float)i / 4096.0f);
    for (int i = 0; i < 128; i++) { g_c128[i] = cosf(Q_TAU * (float)i / 128.0f); g_s128[i] = sinf(Q_TAU * (float)i / 128.0f); }
    for (int m = 0, o = 0; m < Q_AM_MIPS; m++) { g_am_off[m] = o; o += AM_N[m] + 1; }
    int off = 0;
    for (int m = 0; m < Q_MIPS; m++) {
        g_mip_off[m] = off;
        off += MIP_N[m] + 1;
    }
    for (int t = 0; t < Q_WT_COUNT; t++) {
        static float xr[2048], xi[2048];
        int cosph = Q_WT_COS[t];
        for (int k = 0; k < Q_WT_FRAMES; k++) {
            const float *a = Q_WT_SPEC[t][k];
            float scale = 1.0f;
            for (int m = 0; m < Q_MIPS; m++) {
                int N = MIP_N[m], H = MIP_H[m];
                memset(xr, 0, sizeof(float) * (size_t)N);
                memset(xi, 0, sizeof(float) * (size_t)N);
                for (int h = 1; h <= H; h++) {
                    /* soften the top quarter of each mip so a mip change is not a cliff */
                    float hf = (float)h / (float)H, amp = a[h - 1] * (hf > 0.75f ? 1.0f - (hf - 0.75f) * 3.2f : 1.0f);
                    if (cosph) xr[h] = 0.5f * amp; else xi[h] = -0.5f * amp;
                    xr[N - h] = xr[h]; xi[N - h] = -xi[h];
                }
                ifft(xr, xi, N);
                float *dst = g_wt[t][k] + g_mip_off[m];
                if (m == 0) {
                    float pk = 0.0f;
                    for (int i = 0; i < N; i++) pk = fmaxf(pk, fabsf(xr[i]));
                    scale = pk > 1e-6f ? 0.95f / pk : 1.0f;
                }
                for (int i = 0; i < N; i++) dst[i] = xr[i] * scale;
                dst[N] = dst[0];
            }
        }
    }
    for (int m = 0, o = 0; m < Q_MIPS; m++) { g_ch_off[m] = o; o += CH_N[m] + 1; }
    for (int t = 0; t < Q_CH_COUNT; t++) ch_build(t);
    g_tables_ready = 1;
}

static inline float sin_cyc(float ph) { /* ph in cycles, any value */
    ph -= floorf(ph);
    float x = ph * 4096.0f;
    int i = (int)x;
    float f = x - (float)i;
    return g_sin[i] + (g_sin[i + 1] - g_sin[i]) * f;
}

/* Variation -> interpolated register point for a strings-family type. */
static void reg_at(int s, float v, float *out) {
    int c = Q_STR_COUNT[s], st = Q_STR_START[s];
    float x = q_clamp(v, 0.0f, 1.0f) * (float)(c - 1);
    int i = (int)x;
    if (i >= c - 1) i = c - 2;
    float f = x - (float)i;
    const float *a = Q_REGS[st + i], *b = Q_REGS[st + i + 1];
    for (int j = 0; j < Q_REG_FIELDS; j++) out[j] = a[j] + (b[j] - a[j]) * f;
}

/* ------------------------------------------------------------------ Amino tables, on demand */

/* One 128-sample 8-bit wave -> its 64 harmonics (DFT) -> each mip resynthesised with only the
 * harmonics that mip may hold (top quarter tapered, like the Quark tables). DC is dropped. */
static void am_build_frame(const signed char *src, float *dst) {
    float re[Q_AM_LEN / 2 + 1], im[Q_AM_LEN / 2 + 1];
    for (int h = 1; h <= Q_AM_LEN / 2; h++) {
        float c = 0.0f, d = 0.0f;
        for (int n = 0; n < Q_AM_LEN; n++) {
            int k = (h * n) & (Q_AM_LEN - 1);
            c += (float)src[n] * g_c128[k];
            d += (float)src[n] * g_s128[k];
        }
        float sc = (h == Q_AM_LEN / 2 ? 1.0f : 2.0f) / ((float)Q_AM_LEN * 127.0f);
        re[h] = c * sc;
        im[h] = d * sc;
    }
    for (int m = 0; m < Q_AM_MIPS; m++) {
        int N = AM_N[m], H = AM_H[m], step = 4096 / N;
        float *o = dst + g_am_off[m];
        for (int i = 0; i < N; i++) {
            float acc = 0.0f;
            for (int h = 1; h <= H; h++) {
                float hf = (float)h / (float)H, t = hf > 0.75f && H > 2 ? 1.0f - (hf - 0.75f) * 3.2f : 1.0f;
                int k = (h * i * step) & 4095;
                acc += t * (re[h] * g_sin[(k + 1024) & 4095] + im[h] * g_sin[k]);
            }
            o[i] = acc;
        }
        o[N] = o[0];
    }
}

void q_wave_update(QInstance *q, int slot, int wt) {
    if (wt < Q_NUM_WT_QUARK || wt >= Q_NUM_WT) return;
    int id = wt - Q_NUM_WT_QUARK;
    QAmSlot *s = &q->am[slot];
    if (s->have[s->front] == id) { s->build_id = -1; return; }
    int back = s->front ^ 1;
    if (s->have[back] == id && s->build_id < 0) { s->front = back; return; }
    /* 16 waves per block (~4 blocks per table) keeps the build far below a block's budget even on the
     * Move; until the first table is ready that layer is silent for those few milliseconds */
    if (s->build_id != id) { s->build_id = id; s->build_k = 0; s->have[back] = -1; }
    for (int todo = 16; todo > 0 && s->build_k < Q_AM_WAVES; todo--, s->build_k++)
        am_build_frame(quark_amino_wt[id][s->build_k], s->buf[back][s->build_k]);
    if (s->build_k >= Q_AM_WAVES) { s->have[back] = id; s->front = back; s->build_id = -1; }
}

int q_wave_get(QInstance *q, int slot, int wt, QWave *w) {
    w->div = 1.0f;
    if (wt < Q_WT_COUNT) {
        w->base = g_wt[q_clampi(wt, 0, Q_WT_COUNT - 1)][0];
        w->nf = Q_WT_FRAMES; w->stride = Q_FRAME_FLOATS; w->nm = Q_MIPS;
        w->off = g_mip_off; w->len = MIP_N; w->harm = MIP_H;
        return 1;
    }
    if (wt < Q_NUM_WT_QUARK) {
        int t = wt - Q_WT_COUNT;
        w->base = g_ch[t][0];
        w->nf = Q_WT_FRAMES; w->stride = Q_CH_FF; w->nm = Q_MIPS;
        w->off = g_ch_off; w->len = CH_N; w->harm = g_ch_harm[t];
        w->div = (float)Q_CH_DIV[t];
        return 1;
    }
    QAmSlot *s = &q->am[slot];
    if (s->have[s->front] < 0) return 0;   /* (while a new table builds, the old one keeps playing) */
    w->base = s->buf[s->front][0];
    w->nf = Q_AM_WAVES; w->stride = Q_AM_FF; w->nm = Q_AM_MIPS;
    w->off = g_am_off; w->len = AM_N; w->harm = AM_H;
    return 1;
}

/* ------------------------------------------------------------------ voices */

static const float SPREAD_PAT[Q_MAX_TEX] = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.75f, -0.75f, 0.25f, -0.25f, 0.9f, -0.9f, 0.6f};
/* Detune: every voice of a chord gets its own offset (none zero, signs alternate), so the chord
 * smears apart; Bloom slides each note from in tune to that offset after note-on. */
static const float DET_PAT[Q_MAX_TEX] = {0.62f, -0.85f, 0.33f, -0.48f, 1.0f, -0.2f, 0.78f, -0.66f, 0.15f, -1.0f, 0.5f, -0.36f};

static void voice_start(QInstance *q, QTexVoice *v, int key, int note, float vel) {
    int fresh = !v->active;
    v->active = 1;
    v->gate = 1;
    v->sustained = 0;
    v->key = key;
    v->note = note;
    v->vel = vel;
    v->age = ++q->age;
    v->kill = 0;
    v->amp_st = 1;
    v->mod_st = 1;
    v->ndet = q_randb(&q->rng);
    /* vintage tolerances: every note its own cutoff, envelope speed and level */
    v->ncut = q_randb(&q->rng);
    v->nenv = q_randb(&q->rng);
    v->nlev = q_randb(&q->rng);
    v->tine = 1.0f;
    v->idx1 = -1.0f;
    v->bloom = 0.0f;
    if (fresh) {
        v->amp = 0.0f;
        v->mod = 0.0f;
        /* free-running phases: a string machine never resets its dividers */
        v->ph = (float)(q_rand(&q->rng) & 0xFFFF) / 65536.0f;
        v->ph2 = (float)(q_rand(&q->rng) & 0xFFFF) / 65536.0f;
        v->ph3 = 0.0f;
        v->fbm = 0.0f;
        v->s1[0] = v->s1[1] = v->s2[0] = v->s2[1] = 0.0f;
        v->tone = 0.0f;
        v->dx = v->dF = v->dlp = 0.0f;
        v->gl = v->gr = 0.0f;
        v->cdrift = 0.0f;
        v->comb_d = -1.0f;
        v->comb_lp = 0.0f;
        v->comb_w = 0;
        memset(q->comb[v - q->tv], 0, sizeof q->comb[0]);
        v->wpos = -1.0f;   /* snap on the first control tick */
        for (int j = 0; j < 7; j++) v->g[j] = -1.0f;
    }
    /* FM: restart the modulators so every note's attack has the same bite */
    if (Q_TEX_FAMILY[q_clampi((int)q->ps[P_TX_TYPE], 0, Q_NUM_TEX_TYPES - 1)] == 2) {
        v->ph2 = 0.0f;
        v->ph3 = 0.0f;
        v->fbm = 0.0f;
    }
}

void q_tex_note_on(QInstance *q, int key, int note, float vel) {
    int nv = q_clampi((int)(q->pv[P_TX_VOICES] + 0.5f), 2, Q_MAX_TEX);
    QTexVoice *best = 0;
    /* same key retrigger */
    for (int i = 0; i < nv; i++)
        if (q->tv[i].active && q->tv[i].key == key && !q->tv[i].kill) { best = &q->tv[i]; break; }
    if (!best)
        for (int i = 0; i < nv; i++)
            if (!q->tv[i].active) { best = &q->tv[i]; break; }
    if (best) {
        best->slot = (int)(best - q->tv);
        voice_start(q, best, key, note, vel);
        return;
    }
    /* steal: the quietest released voice, else the oldest */
    uint32_t oldest = 0xFFFFFFFFu;
    float quiet = 2.0f;
    for (int i = 0; i < nv; i++) {
        QTexVoice *v = &q->tv[i];
        if (v->kill) continue;
        if (!v->gate && v->amp < quiet) { quiet = v->amp; best = v; }
    }
    if (!best)
        for (int i = 0; i < nv; i++) {
            QTexVoice *v = &q->tv[i];
            if (v->kill) continue;
            if (v->age < oldest) { oldest = v->age; best = v; }
        }
    if (!best) best = &q->tv[0];
    best->kill = 1;
    best->pend_key = key;
    best->pend_note = note;
    best->pend_vel = vel;
}

void q_tex_note_off(QInstance *q, int key) {
    for (int i = 0; i < Q_MAX_TEX; i++) {
        QTexVoice *v = &q->tv[i];
        if (v->kill && v->pend_key == key) { v->pend_key = -1; continue; }
        if (!v->active || !v->gate || v->key != key) continue;
        if (q->sustain) { v->sustained = 1; continue; }
        v->gate = 0;
        v->amp_st = 3;
        v->mod_st = 3;
    }
}

/* ------------------------------------------------------------------ render */

static inline float lfo_shape(int shape, float ph, float held, float prev) {
    switch (shape) {
    case 0: return sin_cyc(ph);
    case 1: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
    case 2: return 2.0f * ph - 1.0f;
    case 3: return ph < 0.5f ? 1.0f : -1.0f;
    case 4: return held;
    default: { float s = ph * ph * (3.0f - 2.0f * ph); return prev + (held - prev) * s; }
    }
}

void q_tex_render(QInstance *q, float *outl, float *outr, int n) {
    const float *P = q->ps;
    float sr = q->sr;
    int type = q_clampi((int)(P[P_TX_TYPE] + 0.5f), 0, Q_NUM_TEX_TYPES - 1);
    int fam = Q_TEX_FAMILY[type], ix = Q_TEX_INDEX[type];
    /* 0 Off 1 LP12 2 LP24 3 HP12 4 BP12 5 BP24 6 Dual BP 7 Notch 8 Comb+ 9 Comb- 10 Morph */
    int ftype = q_clampi((int)(P[P_FLT_TYPE] + 0.5f), 0, Q_NUM_FILTER_TYPES - 1);
    int comb = ftype == 8 || ftype == 9;
    int dest = q_clampi((int)(P[P_AN_DEST] + 0.5f), 0, 4);
    int wheel_dst = q_clampi((int)(P[P_WHEEL] + 0.5f), 0, 3);
    int ticks = (n + Q_CTRL - 1) / Q_CTRL;
    float flav = P[P_VINTAGE], spread = P[P_TX_SPREAD], velsens = P[P_VEL];
    float sus_t = P[P_TX_S], sus_m = P[P_ME_S];

    /* ---- global modulators per control tick ---- */
    float an_t[Q_BLOCK / Q_CTRL], vib_t[Q_BLOCK / Q_CTRL];
    float an_hz = 0.03f * powf(333.0f, P[P_AN_RATE]);
    float vib_hz = 3.0f + 6.0f * P[P_VIB_RATE];
    int shape = q_clampi((int)(P[P_AN_SHAPE] + 0.5f), 0, 5);
    for (int t = 0; t < ticks; t++) {
        q->an_ph += an_hz * (float)Q_CTRL / sr;
        if (q->an_ph >= 1.0f) {
            q->an_ph -= 1.0f;
            q->an_prev = q->an_next;
            q->an_next = q_randb(&q->rng);
        }
        an_t[t] = lfo_shape(shape, q->an_ph, q->an_next, q->an_prev);
        q->vib_ph += vib_hz * (float)Q_CTRL / sr;
        if (q->vib_ph >= 1.0f) q->vib_ph -= 1.0f;
        vib_t[t] = sin_cyc(q->vib_ph);
    }
    float anim = P[P_TX_ANIM] + (wheel_dst == 1 ? q->wheel : 0.0f);
    float vib_amt = P[P_VIB] * 0.5f + q->at * 0.4f + (wheel_dst == 0 ? q->wheel * 0.6f : 0.0f);
    float var_base = P[P_TX_VAR] + (wheel_dst == 3 ? q->wheel * 0.6f : 0.0f);
    q->var_eff = q_clamp(var_base + (dest == 0 ? anim * 0.5f * an_t[0] : 0.0f), 0.0f, 1.0f);
    float cut_base = 20.0f + 115.0f * P[P_FLT_CUT] + (wheel_dst == 2 ? q->wheel * 40.0f : 0.0f);
    q->cut_eff = cut_base + (dest == 2 ? anim * 36.0f * an_t[0] : 0.0f);

    float base_pitch = 12.0f * (float)q_clampi((int)lroundf(P[P_TX_OCT]), -2, 2) + q->bend * 2.0f;
    float res = P[P_FLT_RES];
    float fk_base = 2.0f - 1.94f * res; /* SVF damping, 2 = no resonance */
    float tx_vol = P[P_TX_VOL] * 0.26f;
    float k_ctrl = (float)Q_CTRL;
    float kill_c = 1.0f - expf(-1.0f / (0.0015f * sr));
    const float *fmr = Q_FM[fam == 2 ? ix : 0];
    QWave wv;
    int have_wave = 0;
    if (fam == 1) {
        q_wave_update(q, 0, ix);
        have_wave = q_wave_get(q, 0, ix, &wv);
    }
    /* drive (Pre / Post filter run per voice here; Amp runs on the bus) */
    int dpos = q_clampi((int)(P[P_TX_DRVPOS] + 0.5f), 0, 2);
    QDrive drv = {0};
    int use_drv = P[P_TX_DRIVE] > 0.003f && dpos < 2;
    if (use_drv) q_drive_set(&drv, P[P_TX_DRIVE], P[P_TX_DTONE], 1.0f, 0.8f, sr);
    float detune = P[P_TX_DETUNE], det_ct = detune * detune * 45.0f;
    float blm = P[P_TX_BLOOM];
    float bloom_inc = blm < 0.005f ? 1.0f : (float)Q_CTRL / ((0.05f + 15.0f * blm * blm) * sr);
    int partner = spread > 0.01f || detune > 0.01f;
    float tine_c = (fam == 2 && fmr[4] > 0.0f) ? expf(-(float)Q_CTRL / (fmr[4] * sr)) : 1.0f;

    for (int vi = 0; vi < Q_MAX_TEX; vi++) {
        QTexVoice *v = &q->tv[vi];
        if (!v->active) continue;
        float pat = SPREAD_PAT[vi];
        for (int t = 0; t < ticks; t++) {
            int i0 = t * Q_CTRL;
            int m = n - i0 < Q_CTRL ? n - i0 : Q_CTRL;
            float an = an_t[t];
            /* ---- control: pitch ---- */
            v->drift += (q_randb(&q->rng) * 0.05f - v->drift * 0.002f);
            v->drift = q_clamp(v->drift, -1.0f, 1.0f);
            v->cdrift += (q_randb(&q->rng) * 0.05f - v->cdrift * 0.002f);
            v->cdrift = q_clamp(v->cdrift, -1.0f, 1.0f);
            v->bloom = v->bloom + bloom_inc > 1.0f ? 1.0f : v->bloom + bloom_inc;
            float cents = flav * (6.0f * v->drift + 4.0f * v->ndet) + spread * 9.0f * pat +
                          det_ct * DET_PAT[vi] * (0.85f + 0.15f * v->ndet) * v->bloom;
            float pitch = (float)v->note + base_pitch + cents * 0.01f + vib_amt * vib_t[t] +
                          (dest == 1 ? anim * 0.6f * an : 0.0f);
            float hz = q_note_hz(pitch);
            /* ---- control: variation ---- */
            float var = var_base + (dest == 0 ? anim * 0.5f * an : 0.0f) + P[P_TX_DEPTH] * v->mod;
            var = q_clamp(var, 0.0f, 1.0f);
            float inv_m = 1.0f / (float)m;
            float bright = 0.0f, swell = 0.0f;
            if (fam == 0) {
                float r[Q_REG_FIELDS];
                reg_at(ix, var, r);
                float norm = 0.0f;
                for (int j = 0; j < 7; j++) norm += r[j] * r[j];
                norm = norm > 1e-6f ? 0.75f / sqrtf(norm) : 0.0f;
                for (int j = 0; j < 7; j++) {
                    float tgt = r[j] * norm;
                    if (v->g[j] < 0.0f) v->g[j] = tgt;
                    v->dg[j] = (tgt - v->g[j]) * inv_m;
                }
                bright = r[7];
                swell = r[10];
                v->inc = 0.5f * hz / sr;
                float tc = q_note_hz((float)v->note + base_pitch + bright + swell * v->mod);
                v->toneg = 1.0f - expf(-Q_TAU * q_clamp(tc, 20.0f, 0.45f * sr) / sr);
            } else if (fam == 1) {
                float pos = q_clamp(var + flav * 0.03f * v->drift, 0.0f, 1.0f);
                if (v->wpos < 0.0f) v->wpos = pos;
                v->dwpos = (pos - v->wpos) * inv_m;
                float whz = have_wave ? hz / wv.div : hz;   /* the table's own cycle rate */
                v->inc = whz / sr;
                float pc = spread * 14.0f + detune * detune * 20.0f * v->bloom;
                v->inc2 = partner ? v->inc * exp2f(pc / 1200.0f) : 0.0f;
                v->mip = have_wave ? q_wave_mip(&wv, whz * (1.0f + pc * 0.0006f), sr) : 0;
            } else {
                v->inc = hz / sr;
                v->inc2 = v->inc * fmr[0];
                v->inc3 = v->inc * fmr[2];
                /* keep the sidebands under Nyquist: fade the indices out as they would alias */
                float lim1 = q_clamp((0.42f * sr - hz * fmr[0] * (1.0f + var * fmr[1])) / (0.2f * sr), 0.0f, 1.0f);
                float lim2 = q_clamp((0.42f * sr - hz * fmr[2] * 2.0f) / (0.2f * sr), 0.0f, 1.0f);
                float tgt1 = var * fmr[1] * lim1;
                if (v->idx1 < 0.0f) v->idx1 = tgt1;
                v->didx1 = (tgt1 - v->idx1) * inv_m;
                v->tine *= tine_c;
                v->idx2 = fmr[3] * lim2 * (fmr[4] > 0.0f ? v->tine : 1.0f);
            }
            /* ---- control: filter ---- */
            if (ftype > 0) {
                float cn = cut_base + (dest == 2 ? anim * 36.0f * an : 0.0f) +
                           P[P_FLT_KEY] * ((float)v->note - 60.0f) + P[P_FLT_ENV] * 72.0f * v->mod +
                           velsens * 24.0f * (v->vel - 0.6f) +
                           flav * (1.5f * v->ncut + 1.0f * v->cdrift); /* vintage: +-2.5 st at most */
                float fc = q_clamp(q_note_hz(cn), 16.0f, 0.46f * sr);
                if (comb) {
                    /* the line is one period of the cutoff; Resonance is the feedback */
                    float d = q_clamp(sr / fc, 2.0f, (float)(Q_COMB_N - 4));
                    if (v->comb_d < 0.0f) v->comb_d = d;
                    v->comb_dd = (d - v->comb_d) * inv_m;
                    float fb = 0.3f + 0.66f * res;
                    v->comb_fb = ftype == 8 ? fb : -fb;
                } else {
                    v->fg = tanf(Q_PI * fc / sr);
                    v->fk = (ftype == 2 || ftype == 5) ? 2.0f - 1.6f * res
                          : ftype == 6 ? 2.0f - 1.8f * res
                          : ftype == 10 ? 1.0f : fk_base;  /* Morph: Resonance is the morph */
                    if (ftype == 6) v->fg2 = tanf(Q_PI * q_clamp(fc * 2.5f, 16.0f, 0.46f * sr) / sr);
                }
            }
            /* ---- control: output gains ---- */
            float vgain = tx_vol * (1.0f - velsens + velsens * v->vel) * (1.0f + 0.12f * flav * v->nlev);
            if (dest == 4) vgain *= 1.0f - anim * (0.5f + 0.5f * an);
            float pan = q_clamp(spread * 0.55f * pat + (dest == 3 ? anim * an : 0.0f), -1.0f, 1.0f);
            float pa = (pan + 1.0f) * (Q_PI * 0.25f);
            float tgl = cosf(pa) * vgain * 1.4142f, tgr = sinf(pa) * vgain * 1.4142f;
            v->dgl = (tgl - v->gl) * inv_m;
            v->dgr = (tgr - v->gr) * inv_m;

            /* ---- audio ---- */
            float buf[Q_CTRL];
            if (fam == 0) {
                float ph = v->ph, inc = v->inc, tone = v->tone, tg = v->toneg;
                float d16 = inc, d8 = 2.0f * inc, d4 = 4.0f * inc, d2 = 8.0f * inc;
                float g0 = v->g[0], g1 = v->g[1], g2 = v->g[2], g3 = v->g[3], g4 = v->g[4], g5 = v->g[5], g6 = v->g[6];
                int use_sq = (g3 + g4 + g5 + g6 + v->dg[3] + v->dg[4] + v->dg[5] + v->dg[6]) > 1e-5f;
                for (int i = 0; i < m; i++) {
                    ph += inc;
                    if (ph >= 1.0f) ph -= 1.0f;
                    float p8 = ph * 2.0f; p8 -= (float)(int)p8;
                    float p4 = ph * 4.0f; p4 -= (float)(int)p4;
                    float x = 0.0f;
                    if (g0 > 0.0f) x += g0 * (2.0f * ph - 1.0f - q_blep(ph, d16));
                    if (g1 > 0.0f) x += g1 * (2.0f * p8 - 1.0f - q_blep(p8, d8));
                    if (g2 > 0.0f) x += g2 * (2.0f * p4 - 1.0f - q_blep(p4, d4));
                    if (use_sq) {
                        float p2 = ph * 8.0f; p2 -= (float)(int)p2;
                        float h;
                        if (g3 > 0.0f) { h = ph + 0.5f; h -= (float)(int)h; x += g3 * ((ph < 0.5f ? 1.0f : -1.0f) + q_blep(ph, d16) - q_blep(h, d16)); }
                        if (g4 > 0.0f) { h = p8 + 0.5f; h -= (float)(int)h; x += g4 * ((p8 < 0.5f ? 1.0f : -1.0f) + q_blep(p8, d8) - q_blep(h, d8)); }
                        if (g5 > 0.0f) { h = p4 + 0.5f; h -= (float)(int)h; x += g5 * ((p4 < 0.5f ? 1.0f : -1.0f) + q_blep(p4, d4) - q_blep(h, d4)); }
                        if (g6 > 0.0f && d2 < 0.5f) { h = p2 + 0.5f; h -= (float)(int)h; x += g6 * ((p2 < 0.5f ? 1.0f : -1.0f) + q_blep(p2, d2) - q_blep(h, d2)); }
                    }
                    g0 += v->dg[0]; g1 += v->dg[1]; g2 += v->dg[2]; g3 += v->dg[3];
                    g4 += v->dg[4]; g5 += v->dg[5]; g6 += v->dg[6];
                    tone += (x - tone) * tg;
                    buf[i] = tone;
                }
                v->ph = ph;
                v->tone = tone;
                v->g[0] = g0; v->g[1] = g1; v->g[2] = g2; v->g[3] = g3; v->g[4] = g4; v->g[5] = g5; v->g[6] = g6;
            } else if (fam == 1) {
                float ph = v->ph, ph2 = v->ph2, inc = v->inc, inc2 = v->inc2, wp = v->wpos, dw = v->dwpos;
                int mp = v->mip;
                for (int i = 0; i < m; i++) {
                    ph += inc;
                    if (ph >= 1.0f) ph -= 1.0f;
                    float x = have_wave ? q_wave_read(&wv, mp, wp, ph) : 0.0f;
                    if (inc2 > 0.0f) {
                        ph2 += inc2;
                        if (ph2 >= 1.0f) ph2 -= 1.0f;
                        x = 0.62f * (x + (have_wave ? q_wave_read(&wv, mp, wp, ph2) : 0.0f));
                    }
                    buf[i] = x * 0.8f;
                    wp += dw;
                }
                v->ph = ph;
                v->ph2 = ph2;
                v->wpos = wp;
            } else {
                float ph = v->ph, p2 = v->ph2, p3 = v->ph3, fbm = v->fbm, i1 = v->idx1;
                float fb = fmr[5] * 0.25f, i2 = v->idx2 * (1.0f / Q_TAU);
                for (int i = 0; i < m; i++) {
                    ph += v->inc; ph -= (float)(int)ph;
                    p2 += v->inc2; p2 -= (float)(int)p2;
                    p3 += v->inc3; p3 -= (float)(int)p3;
                    float o1 = sin_cyc(p2 + fbm * fb);
                    fbm = o1;
                    float o2 = sin_cyc(p3);
                    buf[i] = 0.8f * sin_cyc(ph + o1 * i1 * (1.0f / Q_TAU) + o2 * i2);
                    i1 += v->didx1;
                }
                v->ph = ph; v->ph2 = p2; v->ph3 = p3; v->fbm = fbm; v->idx1 = i1;
            }

            if (use_drv && dpos == 0) q_drive_run(&drv, buf, m, &v->dx, &v->dF, &v->dlp);

            /* ---- filter ---- */
            if (comb) {
                float *cb = q->comb[vi];
                int w = v->comb_w;
                float d = v->comb_d, dd = v->comb_dd, lp = v->comb_lp, fb = v->comb_fb;
                float cg = 1.0f - 0.6f * fabsf(fb);
                for (int i = 0; i < m; i++) {
                    d += dd;
                    float rp = (float)w - d;
                    if (rp < 0.0f) rp += (float)Q_COMB_N;
                    int r0 = (int)rp;
                    float fr = rp - (float)r0;
                    r0 &= Q_COMB_N - 1;
                    float y0 = cb[r0] + (cb[(r0 + 1) & (Q_COMB_N - 1)] - cb[r0]) * fr;
                    lp += (y0 - lp) * 0.65f;   /* loop damping: the high harmonics ring shorter */
                    float y = buf[i] + fb * lp;
                    cb[w] = y;
                    w = (w + 1) & (Q_COMB_N - 1);
                    buf[i] = y * cg;
                }
                v->comb_w = w; v->comb_d = d; v->comb_lp = lp;
            } else if (ftype == 6) {
                /* Dual BP: two band-passes, the second 2.5x higher (a vowel-like pair) */
                float g = v->fg, g2 = v->fg2, kk = v->fk;
                float a1 = 1.0f / (1.0f + g * (g + kk)), b1 = 1.0f / (1.0f + g2 * (g2 + kk));
                float ic1 = v->s1[0], ic2 = v->s2[0], jc1 = v->s1[1], jc2 = v->s2[1];
                for (int i = 0; i < m; i++) {
                    float x = buf[i];
                    float v1 = (ic1 + g * (x - ic2)) * a1, v2 = ic2 + g * v1;
                    ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
                    float u1 = (jc1 + g2 * (x - jc2)) * b1, u2 = jc2 + g2 * u1;
                    jc1 = 2.0f * u1 - jc1; jc2 = 2.0f * u2 - jc2;
                    buf[i] = kk * (v1 + u1);
                }
                v->s1[0] = ic1; v->s2[0] = ic2; v->s1[1] = jc1; v->s2[1] = jc2;
            } else if (ftype == 10) {
                /* Morph: LP > BP > HP from one SVF, Resonance is the position */
                float g = v->fg, kk = v->fk, mo = q_clamp(res, 0.0f, 1.0f);
                float a1 = 1.0f / (1.0f + g * (g + kk));
                float wl = mo < 0.5f ? 1.0f - 2.0f * mo : 0.0f;
                float wb = mo < 0.5f ? 2.0f * mo : 2.0f - 2.0f * mo;
                float wh = mo < 0.5f ? 0.0f : 2.0f * mo - 1.0f;
                float ic1 = v->s1[0], ic2 = v->s2[0];
                for (int i = 0; i < m; i++) {
                    float x = buf[i];
                    float v1 = (ic1 + g * (x - ic2)) * a1, v2 = ic2 + g * v1;
                    ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
                    buf[i] = wl * v2 + wb * kk * v1 + wh * (x - kk * v1 - v2);
                }
                v->s1[0] = ic1; v->s2[0] = ic2;
            } else if (ftype > 0) {
                /* LP12 / LP24 / HP12 / BP12 / BP24 / Notch: one or two cascaded TPT SVF stages */
                float g = v->fg, kk = v->fk;
                float a1 = 1.0f / (1.0f + g * (g + kk));
                int stages = (ftype == 2 || ftype == 5) ? 2 : 1;
                int out = ftype <= 2 ? 0 : ftype == 3 ? 2 : ftype == 7 ? 3 : 1;   /* 0 LP 1 BP 2 HP 3 notch */
                for (int s = 0; s < stages; s++) {
                    float ic1 = v->s1[s], ic2 = v->s2[s];
                    float bpg = kk * (s == stages - 1 ? 1.6f : 1.0f);   /* band-pass make-up, once */
                    for (int i = 0; i < m; i++) {
                        float x = buf[i];
                        float v1 = (ic1 + g * (x - ic2)) * a1;
                        float v2 = ic2 + g * v1;
                        ic1 = 2.0f * v1 - ic1;
                        ic2 = 2.0f * v2 - ic2;
                        buf[i] = out == 0 ? v2 : out == 1 ? bpg * v1 : out == 2 ? x - kk * v1 - v2 : x - kk * v1;
                    }
                    v->s1[s] = ic1;
                    v->s2[s] = ic2;
                }
            }

            if (use_drv && dpos == 1) q_drive_run(&drv, buf, m, &v->dx, &v->dF, &v->dlp);

            /* ---- envelopes + output ---- */
            float amp = v->amp, mod = v->mod, gl = v->gl, gr = v->gr;
            int ast = v->amp_st, mst = v->mod_st;
            /* vintage: each note's envelopes run up to +-30% faster or slower */
            float ek = 1.0f + 0.3f * flav * v->nenv;
            float eac = q_clamp(q->tx_ac * ek, 0.0f, 1.0f), edc = q_clamp(q->tx_dc * ek, 0.0f, 1.0f);
            float erc = q_clamp(q->tx_rc * ek, 0.0f, 1.0f);
            for (int i = 0; i < m; i++) {
                if (v->kill) {
                    amp -= amp * kill_c;
                } else if (ast == 1) {
                    amp += (1.25f - amp) * eac;
                    if (amp >= 1.0f) { amp = 1.0f; ast = 2; }
                } else if (ast == 2) {
                    amp += (sus_t - amp) * edc;
                } else {
                    amp -= amp * erc;
                }
                gl += v->dgl;
                gr += v->dgr;
                float s = buf[i] * amp;
                outl[i0 + i] += s * gl;
                outr[i0 + i] += s * gr;
            }
            /* the mod env only feeds control-rate destinations: advance it per tick */
            if (mst == 1) {
                float c = 1.0f - powf(1.0f - q_clamp(q->me_ac * ek, 0.0f, 1.0f), k_ctrl);
                mod += (1.25f - mod) * c;
                if (mod >= 1.0f) { mod = 1.0f; mst = 2; }
            } else if (mst == 2) {
                mod += (sus_m - mod) * (1.0f - powf(1.0f - q_clamp(q->me_dc * ek, 0.0f, 1.0f), k_ctrl));
            } else if (mst == 3) {
                mod -= mod * (1.0f - powf(1.0f - q_clamp(q->me_rc * ek, 0.0f, 1.0f), k_ctrl));
            }
            v->amp = amp; v->mod = mod; v->gl = gl; v->gr = gr;
            v->amp_st = ast; v->mod_st = mst;

            if (v->kill && amp < 0.002f) {
                v->kill = 0;
                v->amp = 0.0f;
                if (v->pend_key >= 0) {
                    v->active = 0;
                    voice_start(q, v, v->pend_key, v->pend_note, v->pend_vel);
                } else {
                    v->active = 0;
                    break;
                }
            } else if (ast == 3 && amp < 0.0002f) {
                v->active = 0;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ bus: formants + ensemble */

static const float VOWEL_F[5][2] = {{300, 870}, {570, 840}, {730, 1090}, {530, 1840}, {270, 2290}};

static inline float ens_read(const float *b, int w, float d) {
    float pos = (float)w - d;
    if (pos < 0.0f) pos += (float)Q_ENS_N;
    int i = (int)pos;
    float f = pos - (float)i;
    int i1 = (i + 1) & (Q_ENS_N - 1);
    return b[i & (Q_ENS_N - 1)] * (1.0f - f) + b[i1] * f;
}

void q_tex_bus(QInstance *q, float *l, float *r, int n) {
    const float *P = q->ps;
    QFx *fx = &q->fx;
    float sr = q->sr;
    int type = q_clampi((int)(P[P_TX_TYPE] + 0.5f), 0, Q_NUM_TEX_TYPES - 1);

    /* formant bank: strings family only, follows the global (not per-voice) Variation */
    if (Q_TEX_FAMILY[type] == 0) {
        float rr[Q_REG_FIELDS];
        reg_at(Q_TEX_INDEX[type], q->var_eff, rr);
        float amt = rr[8];
        if (amt > 0.001f) {
            float vx = q_clamp(rr[9], 0.0f, 1.0f) * 4.0f;
            int vi = (int)vx;
            if (vi > 3) vi = 3;
            float vf = vx - (float)vi;
            float f1 = VOWEL_F[vi][0] + (VOWEL_F[vi + 1][0] - VOWEL_F[vi][0]) * vf;
            float f2 = VOWEL_F[vi][1] + (VOWEL_F[vi + 1][1] - VOWEL_F[vi][1]) * vf;
            float g1 = tanf(Q_PI * f1 / sr), g2 = tanf(Q_PI * f2 / sr);
            float k1 = 0.22f, k2 = 0.18f;
            float a1 = 1.0f / (1.0f + g1 * (g1 + k1)), a2 = 1.0f / (1.0f + g2 * (g2 + k2));
            float dry = 1.0f - 0.8f * amt;
            for (int c = 0; c < 2; c++) {
                float *x = c ? r : l;
                float s11 = fx->fm1[c][0], s12 = fx->fm1[c][1], s21 = fx->fm2[c][0], s22 = fx->fm2[c][1];
                for (int i = 0; i < n; i++) {
                    float in = x[i];
                    float v1 = (s11 + g1 * (in - s12)) * a1;
                    float v2 = s12 + g1 * v1;
                    s11 = 2.0f * v1 - s11; s12 = 2.0f * v2 - s12;
                    float w1 = (s21 + g2 * (in - s22)) * a2;
                    float w2 = s22 + g2 * w1;
                    s21 = 2.0f * w1 - s21; s22 = 2.0f * w2 - s22;
                    x[i] = in * dry + amt * (1.7f * k1 * v1 + 1.3f * k2 * w1) * 2.2f;
                }
                fx->fm1[c][0] = s11; fx->fm1[c][1] = s12; fx->fm2[c][0] = s21; fx->fm2[c][1] = s22;
            }
        }
    }

    /* Drive in the Amp position: the whole chord through one stage, so the notes intermodulate */
    if (P[P_TX_DRIVE] > 0.003f && (int)(P[P_TX_DRVPOS] + 0.5f) == 2) {
        QDrive d;
        q_drive_set(&d, P[P_TX_DRIVE], P[P_TX_DTONE], 2.5f, 0.4f, sr);
        q_drive_run(&d, l, n, &fx->amp_x[0], &fx->amp_F[0], &fx->amp_lp[0]);
        q_drive_run(&d, r, n, &fx->amp_x[1], &fx->amp_F[1], &fx->amp_lp[1]);
    }

    /* Ensemble: one mono BBD line, three taps 120 degrees apart (classic string-ensemble style), plus a
     * Juno-style chorus sweep for the Chorus and Both modes. */
    float amt = P[P_ENS_AMT];
    if (amt < 0.001f) return;
    int mode = q_clampi((int)(P[P_ENS_TYPE] + 0.5f), 0, Q_NUM_ENS_TYPES - 1);
    /* Deep: the Both topology with ~3x the depth at half the rate (about +-25 cents at full
     * Amount), for drones: held chords smear and beat against themselves */
    int deep = mode == 3;
    float ms = sr * 0.001f;
    float slow_hz = deep ? 0.37f : 0.63f, fast_hz = deep ? 4.1f : 5.8f, cho_hz = deep ? 0.23f : 0.52f;
    float slow_d = (mode == 1 ? 0.0f : deep ? 3.5f + 3.0f * amt : 1.4f + 0.6f * amt) * ms;
    float fast_d = (mode == 1 ? 0.0f : deep ? 0.3f : 0.22f) * ms;
    float cho_d = (mode == 0 ? 0.0f : deep ? 2.5f : 1.6f) * ms;
    float base = (mode == 1 ? 3.5f : deep ? 11.0f : 6.0f) * ms;
    if (deep) mode = 2;
    float lpc = 1.0f - expf(-Q_TAU * 9000.0f / sr);
    float wet = amt * (mode == 1 ? 0.9f : 1.0f);
    float dry = 1.0f - 0.55f * amt;
    float *b = fx->ens_buf;
    int w = fx->ens_w;
    for (int i0 = 0; i0 < n; i0 += Q_CTRL) {
        int m = n - i0 < Q_CTRL ? n - i0 : Q_CTRL;
        /* modulation at control rate, linearly ramped */
        float d0[3], d1[3], dc0, dc1;
        float s0 = fx->ens_ph_slow, f0 = fx->ens_ph_fast, c0 = fx->ens_ph_cho;
        float s1 = s0 + slow_hz * (float)m / sr, f1 = f0 + fast_hz * (float)m / sr, c1 = c0 + cho_hz * (float)m / sr;
        for (int k = 0; k < 3; k++) {
            float o = (float)k / 3.0f;
            d0[k] = base + slow_d * sin_cyc(s0 + o) + fast_d * sin_cyc(f0 + o);
            d1[k] = base + slow_d * sin_cyc(s1 + o) + fast_d * sin_cyc(f1 + o);
        }
        float tri0 = c0 - floorf(c0), tri1 = c1 - floorf(c1);
        tri0 = tri0 < 0.5f ? 4.0f * tri0 - 1.0f : 3.0f - 4.0f * tri0;
        tri1 = tri1 < 0.5f ? 4.0f * tri1 - 1.0f : 3.0f - 4.0f * tri1;
        dc0 = cho_d * tri0;
        dc1 = cho_d * tri1;
        fx->ens_ph_slow = s1 - floorf(s1);
        fx->ens_ph_fast = f1 - floorf(f1);
        fx->ens_ph_cho = c1 - floorf(c1);
        float inv = 1.0f / (float)m;
        for (int i = 0; i < m; i++) {
            float fr = (float)i * inv;
            int j = i0 + i;
            float in = 0.5f * (l[j] + r[j]);
            b[w] = in;
            float t0 = ens_read(b, w, d0[0] + (d1[0] - d0[0]) * fr);
            float t1 = ens_read(b, w, d0[1] + (d1[1] - d0[1]) * fr);
            float t2 = ens_read(b, w, d0[2] + (d1[2] - d0[2]) * fr);
            float cd = dc0 + (dc1 - dc0) * fr;
            float wl, wr;
            if (mode == 1) {
                wl = ens_read(b, w, base + cd);
                wr = ens_read(b, w, base - cd);
            } else if (mode == 2) {
                float cl = ens_read(b, w, base + 2.0f * ms + cd), cr = ens_read(b, w, base + 2.0f * ms - cd);
                wl = 0.42f * (t0 + 0.6f * t1 - 0.25f * t2) + 0.45f * cl;
                wr = 0.42f * (t2 + 0.6f * t1 - 0.25f * t0) + 0.45f * cr;
            } else {
                wl = 0.62f * (t0 + 0.6f * t1 - 0.25f * t2);
                wr = 0.62f * (t2 + 0.6f * t1 - 0.25f * t0);
            }
            fx->ens_lp[0] += (wl - fx->ens_lp[0]) * lpc;
            fx->ens_lp[1] += (wr - fx->ens_lp[1]) * lpc;
            l[j] = l[j] * dry + fx->ens_lp[0] * wet * 1.25f;
            r[j] = r[j] * dry + fx->ens_lp[1] * wet * 1.25f;
            w = (w + 1) & (Q_ENS_N - 1);
        }
    }
    fx->ens_w = w;
}
