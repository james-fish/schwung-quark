/* Quark - a string / pad / mono synth for Schwung (Ableton Move).
 *
 * Two layers:
 *   Pads     up to 12 voices: divide-down string machine, band-limited wavetables, or 3-op FM,
 *            a resonant filter, Animate LFO, Depth (mod env -> Variation) and the tri-phase Ensemble.
 *   Mono     a 1..4 voice virtual-analog solo synth (saw / pulse / sync, PWM, tremolo, vibrato).
 * Then one multi-effect, a shimmer reverb and Vintage drift over everything.
 *
 * Realtime rules: nothing in render_block allocates, locks or does I/O. All state lives in a
 * statically allocated QInstance; the wavetables are built once, off the audio thread, at create.
 */
#ifndef QUARK_H
#define QUARK_H

#include <math.h>
#include <stdint.h>
#include <stddef.h>

#include "plugin_api.h"
#include "params_gen.h"

#define Q_MAX_TEX 12
#define Q_COMB_N 2048             /* comb line per Pads voice: down to ~24 Hz at 48 kHz */
#define Q_MAX_LINES 4
#define Q_BLOCK 128
#define Q_CTRL 16
#define Q_NUM_SLOTS 4
#define Q_ARP_MAX 32
#define Q_STACK_MAX 16

#define Q_PI 3.14159265358979f
#define Q_TAU 6.28318530717959f

/* ---- texture ------------------------------------------------------------------------------- */
typedef struct {
    int active, gate, key, note, sustained;
    float vel;
    uint32_t age;
    /* oscillators: strings use ph (16' master); WT uses ph/ph2 (unison); FM uses ph carrier,
     * ph2 mod 1, ph3 mod 2 */
    float ph, ph2, ph3, fbm, tine;
    /* envelopes */
    float amp, mod;
    int amp_st, mod_st;           /* 0 off, 1 attack, 2 decay/sustain, 3 release */
    /* filter (two TPT SVF stages) and the string tone one-pole */
    float s1[2], s2[2], tone;
    /* vintage: drift walks + per-note tolerances */
    float drift, ndet;
    float cdrift, ncut, nenv, nlev; /* cutoff walk; cutoff / envelope-time / level tolerance per note */
    int slot;                      /* voice index pattern for spread */
    /* steal: fade out, then start the pending note */
    int kill, pend_key, pend_note;
    float pend_vel;
    /* control-rate values, ramped per sample where it matters */
    float inc, inc2, inc3;
    float g[7], dg[7];             /* string tap gains */
    float wpos, dwpos;             /* wavetable frame position 0..7 */
    float idx1, didx1, idx2;       /* FM indices */
    float fg, fk, toneg;           /* filter g, damping; tone coefficient */
    float fg2;                     /* Dual BP: second band */
    float comb_d, comb_dd, comb_lp, comb_fb; /* comb: delay (ramped), loop damping, feedback */
    int comb_w;
    float gl, gr, dgl, dgr;        /* output gains (amp env applied per sample) */
    int mip;
    float pan;
    float bloom;                   /* 0..1: how far the detune has opened since note-on */
    float dx, dF, dlp;             /* drive: ADAA state + tone one-pole */
} QTexVoice;

/* ---- lines ---------------------------------------------------------------------------------- */
typedef struct {
    int active, gate, key, sustained;
    float vel;
    uint32_t age;
    float pitch, target;           /* note units */
    float p1, p2;                  /* osc phases */
    float env;
    int st;                        /* 0 off 1 attack 2 hold/sustain 3 decay */
    float menv;                    /* mod env (filter + wave): amp attack, 66% of the amp decay */
    int mst;                       /* 1 attack 2 decay (to its sustain level while held) 3 release */
    float y[4];                    /* ladder state */
    float drift;
    float wpos;                    /* wavetable position 0..1 (wavetable shapes) */
    float sp_rnd, sp_slot, pan;    /* Mono Spread: this note's random place, its slot, smoothed pan */
    float cdrift, ncut, nenv;      /* vintage: cutoff walk, per-note cutoff / envelope-time tolerance */
    float gl, gr;                  /* output gains, ramped per sample */
    float t_on;                    /* seconds since note-on, for the vibrato onset */
    float inc1, inc2, cg, ck;      /* control-rate */
    float pw;
} QLineVoice;

/* ---- effects -------------------------------------------------------------------------------- */
typedef struct {
    float *buf;
    int size, w;
} QDelay;

#define Q_MODDLY_N 4096
#define Q_DLY_N (2 * 44100 + 4096)
#define Q_REV_LINE_N 8192
#define Q_REV_DIFF_N 512
#define Q_REV_PRE_N 16384
#define Q_SHIM_N 4096
#define Q_ENS_N 2048

typedef struct {
    /* ensemble (texture bus) */
    float ens_buf[Q_ENS_N];
    int ens_w;
    float ens_ph_slow, ens_ph_fast, ens_ph_cho, ens_lp[2];
    /* formant bank (texture bus) */
    float fm1[2][2], fm2[2][2];    /* [ch][state] */
    /* FX1 */
    float mod_buf[2][Q_MODDLY_N];
    QDelay md[2];
    float dly_buf[2][Q_DLY_N];
    QDelay dd[2];
    float fx_ph, fx_ph2, ap[2][8], ap_fb[2], drv_lp[2], drv_hp[2], dly_lp[2], dly_t, wow_ph;
    int fx_last_type;
    int fx_tail;                   /* blocks of tail left after Amount went to 0 */
    /* reverb */
    float rv_line_buf[8][Q_REV_LINE_N];
    float rv_diff_buf[2][4][Q_REV_DIFF_N];
    float rv_pre_buf[2][Q_REV_PRE_N];
    QDelay rv_line[8], rv_diff[2][4], rv_pre[2];
    float rv_damp[8], rv_mph[8], rv_hp[2];
    int rv_tail;
    /* shimmer pitch shifter */
    float sh_buf[Q_SHIM_N];
    int sh_w;
    float sh_ph, sh_hp, sh_lp, sh_out;
    /* Mono delay (on the Mono bus, before the routing): synced, band-passed echoes */
    float mdl_buf[2][Q_DLY_N];
    QDelay mdl[2];
    float mdl_t, mdl_hp[2], mdl_lp[2];
    int mdl_live;
    /* Pads drive in Amp position (on the pad bus) */
    float amp_x[2], amp_F[2], amp_lp[2];
} QFx;

/* ---- wavetables ------------------------------------------------------------------------------
 * The wave bank: Quark's own tables (8 frames, built once at load) followed by the Amino tables
 * (64 waves of 128 samples). An Amino table is band-limited into mip levels only when a layer selects
 * it, into the back buffer of that layer's slot, a few waves per block, then swapped in. */
#define Q_AM_MIPS 7
#define Q_AM_FF (257 + 129 + 65 + 33 + 33 + 33 + 33)
typedef struct {
    float buf[2][Q_AM_WAVES][Q_AM_FF];
    int have[2];                  /* Amino table held by each buffer, -1 = none */
    int front;
    int build_id, build_k;        /* table being built into the back buffer, next wave */
} QAmSlot;

typedef struct {
    const float *base;            /* frame 0 */
    int nf, stride, nm;           /* frames, floats per frame, mip levels */
    const int *off, *len, *harm;  /* per mip: offset in a frame, length, harmonics */
    float div;                    /* periods of the played note in one table cycle (chord stacks > 1) */
} QWave;

/* ---- user wavetables (userwt.c) -------------------------------------------------------------
 * WAV files in the user folder, scanned at create (names only), then loaded by a worker thread into
 * harmonic spectra. When a layer selects one, the worker band-limits it into that layer's back
 * buffer and swaps it in; the audio thread only reads the front buffer and acknowledges it. */
#define Q_UW_FRAMES 64             /* frames kept per table (longer tables are sampled evenly) */
#define Q_UW_H 256                 /* harmonics kept per frame */
#define Q_UW_NAME 24
typedef struct {
    char name[Q_UW_NAME], shortn[5];
    int nf;                        /* frames */
    float *spec;                   /* nf x Q_UW_H x (cos, sin), normalised for the whole table */
    volatile int ready;            /* 0 loading, 1 ready, -1 unreadable */
} QUserTable;

typedef struct {
    float *buf[2];                 /* Q_UW_FRAMES x Q_FRAME_FLOATS each, allocated by the worker */
    volatile int have[2];          /* user table in each buffer, -1 none */
    volatile int front, want, inuse;
} QUwSlot;

/* ---- arpeggiator ----------------------------------------------------------------------------- */
typedef struct {
    int note[Q_ARP_MAX];          /* keys routed to Mono while the arp is on */
    uint32_t nord[Q_ARP_MAX];     /* their press order */
    int count;
    int snap[Q_ARP_MAX];          /* the last non-empty pool, replayed while Latch is on */
    int snap_n;
    double beat;                  /* free-running clock when the transport is stopped */
    long step;                    /* last step index played */
    int pos, dir;
    int on_note[Q_ARP_MAX];       /* notes currently sounding from the arp (on Mono) */
    double on_off[Q_ARP_MAX];     /* the beat each one ends (gates over 100% overlap) */
    int on_count;
    long emit;                    /* triggered steps since the phrase started (octave cycling) */
    uint32_t mstep;               /* clock steps since the phrase started (variation dice) */
    int running;
} QArp;

typedef struct QInstance {
    int used;
    float sr;
    const host_api_v1_t *host;
    float pv[P_COUNT];            /* parameter targets (set_param) */
    float ps[P_COUNT];            /* smoothed copy, advanced once per block */
    int preset;

    QTexVoice tv[Q_MAX_TEX];
    float comb[Q_MAX_TEX][Q_COMB_N]; /* Pads comb filter lines, one per voice */
    QLineVoice lv[Q_MAX_LINES];
    uint32_t age;
    uint32_t rng;

    /* pads: keys down / sounding (latched or sustained count as sounding), press order */
    uint8_t pad_down[128], pad_on[128];
    uint32_t pad_ord[128], ord;
    int pad_down_n;
    QAmSlot am[2];                /* 0 pads, 1 mono */
    QUserTable uw[Q_UW_MAX];
    int uw_n;
    QUwSlot uws[2];               /* 0 pads, 1 mono */
    void *uw_thread;              /* worker (userwt.c), 0 when none */
    volatile int uw_quit;
    char *cp_dyn;                 /* chain_params naming the user tables, 0 = the static one */

    /* lines mono note stack */
    int stack[Q_STACK_MAX];
    int stack_n;
    uint32_t ln_step;             /* Mono Spread: slot counter, one step per new note */

    /* performance state */
    float bend, wheel, at;
    float last_vel;               /* velocity of the last key, for Arp Velocity = As played */
    int sustain;
    float bpm;
    double beat;                  /* host beat position at this block, or -1 */
    int transport;

    /* global modulators (control rate) */
    float an_ph, an_val, an_prev, an_next, vib_ph, vib_val;
    float an2_ph, an2_prev, an2_next; /* LFO 2 (pair Animate Targets) */
    float lfo_ln_pwm, lfo_ln_trem, lfo_ln_vib;
    float var_eff;                /* last effective Variation, for the live widget */
    float cut_eff;

    /* per-block shared envelope coefficients */
    float tx_ac, tx_dc, tx_rc, me_ac, me_dc, me_rc, ln_ac, ln_dc;

    QArp arp;
    QFx fx;
    float peak;
    float sm_vol;
    float dc_x[2], dc_y[2];       /* output DC blocker */
#ifdef QUARK_PROFILE
    float cpu_us;
#endif
} QInstance;

/* tex.c */
void q_tables_init(void);
void q_tex_note_on(QInstance *q, int key, int note, float vel);
void q_tex_note_off(QInstance *q, int key);
void q_tex_render(QInstance *q, float *l, float *r, int n);
void q_tex_bus(QInstance *q, float *l, float *r, int n);
void q_wave_update(QInstance *q, int slot, int wt);
int q_wave_get(QInstance *q, int slot, int wt, QWave *w);

/* Quark's dense tables: 8 mips of up to 256 harmonics, in one frame of Q_FRAME_FLOATS */
#define Q_FRAME_FLOATS (2049 + 1025 + 513 + 257 + 129 + 65 + 65 + 65)
void q_uw_build_frame(const float *spec, float *dst);
void q_uw_spectrum(const float *x, int n, float *spec);

/* userwt.c */
#define Q_WT_USER0 Q_NUM_WT
void q_uw_open(QInstance *q);
void q_uw_close(QInstance *q);
int q_uw_find(const QInstance *q, const char *name);

/* lines.c */
void q_lines_note_on(QInstance *q, int key, float vel);
void q_lines_note_off(QInstance *q, int key);
void q_lines_render(QInstance *q, float *l, float *r, int n);

/* fx.c */
void q_fx_init(QInstance *q);
void q_fx_clear(QInstance *q);
void q_fx1(QInstance *q, float *l, float *r, int n);
void q_mono_delay(QInstance *q, float *l, float *r, int n);
void q_reverb(QInstance *q, const float *il, const float *ir, float *ol, float *orr, int n);

/* quark.c */
void q_release_all(QInstance *q, int hard);

/* helpers shared by the DSP files */
static inline float q_clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline int q_clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

static inline uint32_t q_rand(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x2545F491u;
    return *s;
}
static inline float q_randb(uint32_t *s) { return (float)(q_rand(s) & 0xFFFFFF) * (2.0f / 16777215.0f) - 1.0f; }

/* Pade tanh, exact at +-3 and bounded. */
static inline float q_tanh(float x) {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* 0..1 knob to seconds for envelopes: 1 ms .. 12 s, cubic. */
static inline float q_env_time(float x) {
    x = q_clamp(x, 0.0f, 1.0f);
    return 0.001f + 12.0f * x * x * x;
}

static inline float q_note_hz(float n) { return 440.0f * exp2f((n - 69.0f) * (1.0f / 12.0f)); }

/* PolyBLEP residual for a unit downward step at phase wrap. */
static inline float q_blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline int q_wave_mip(const QWave *w, float hz, float sr) {
    float hmax = 0.45f * sr / (hz > 1.0f ? hz : 1.0f);
    for (int m = 0; m < w->nm; m++)
        if ((float)w->harm[m] <= hmax) return m;
    return w->nm - 1;
}

/* one sample at normalised position pos (0..1 across the frames) and phase ph, from mip m */
static inline float q_wave_read(const QWave *w, int m, float pos, float ph) {
    float fp = pos * (float)(w->nf - 1);
    int k = (int)fp;
    if (k > w->nf - 2) k = w->nf - 2;
    if (k < 0) k = 0;
    float f = fp - (float)k;
    const float *A = w->base + k * w->stride + w->off[m], *B = A + w->stride;
    int n = w->len[m];
    float x = ph * (float)n;
    int i = (int)x;
    float fr = x - (float)i;
    float a = A[i] + (A[i + 1] - A[i]) * fr, b = B[i] + (B[i + 1] - B[i]) * fr;
    return a + (b - a) * f;
}

/* Soft clip 1.5u - 0.5u^3 (unity at +-1) and its antiderivative, for first-order ADAA drive. */
static inline float q_sclip(float u) { return u > 1.0f ? 1.0f : (u < -1.0f ? -1.0f : 1.5f * u - 0.5f * u * u * u); }
static inline float q_sclipF(float u) {
    float a = fabsf(u);
    if (a > 1.0f) return a - 0.375f;
    float u2 = u * u;
    return 0.75f * u2 - 0.125f * u2 * u2;
}
typedef struct { float pre, bias, off, comp, wet, lpc; } QDrive;
static inline void q_drive_set(QDrive *d, float amt, float tone, float pre, float ref, float sr) {
    float g = (1.0f + 24.0f * amt * amt) * pre;
    d->pre = g;
    d->bias = 0.15f * amt;
    d->off = q_sclip(d->bias);
    float pk = q_sclip(ref * g + d->bias) - d->off;
    d->comp = (pk > 1e-4f ? ref / pk : 1.0f) / (1.0f + 0.6f * amt);
    d->wet = amt * 6.0f > 1.0f ? 1.0f : amt * 6.0f;
    float fc = 700.0f * exp2f(5.0f * tone);
    if (fc > 0.45f * sr) fc = 0.45f * sr;
    d->lpc = 1.0f - expf(-Q_TAU * fc / sr);
}
static inline void q_drive_run(const QDrive *d, float *buf, int n, float *xs, float *Fs, float *lps) {
    float xp = *xs, Fp = *Fs, lp = *lps;
    for (int i = 0; i < n; i++) {
        float u = buf[i] * d->pre + d->bias;
        float F = q_sclipF(u), dd = u - xp;
        float y = fabsf(dd) > 1e-3f ? (F - Fp) / dd : q_sclip(0.5f * (u + xp));
        xp = u;
        Fp = F;
        lp += ((y - d->off) * d->comp - lp) * d->lpc;
        buf[i] += (lp - buf[i]) * d->wet;
    }
    *xs = xp; *Fs = Fp; *lps = lp;
}

#endif
