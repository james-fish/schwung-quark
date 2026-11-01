/* Mono layer (the tx_* / ln_* keys keep the old Texture / Lines names).
 *
 * Two detunable oscillators (saw / pulse / saw+pulse / hard sync), a 4-pole resonant ladder, and an
 * AR envelope that is either a one-shot (Trigger: attack then decay, gate ignored) or held (Sustain:
 * attack, hold while the key is down, then decay). Variation is a one-knob character macro:
 * the first third fades in PWM (pulse) / detune shimmer (saw), the second stereo tremolo, the last
 * delayed vibrato - so the knob sweeps from static to fully animated.
 * Shapes past the four analog ones are wavetables (the whole wave bank, Pads' and Amino's): both
 * oscillators read the table, Variation is the table position and Wave Env moves it with the
 * envelope (plucks that open and close), and the PWM / tremolo / vibrato macro is off.
 * Filter Env and Wave Env follow their own mod envelope: the amp attack, then 66% of the amp decay
 * (Sustain mode: down to half while held), so the tone closes before the note fades. Velocity scales
 * both env amounts (depth: the Velocity knob) and the level (a fixed 9 dB range).
 * Mono and Legato use a last-note stack with glide; Poly runs up to four voices.
 * Mono Spread places notes in the stereo field: below centre each note lands somewhere at random,
 * above centre notes step through fixed slots (Poly: each voice keeps its own), so arps walk around.
 */
#include "quark.h"

#include <math.h>
#include <string.h>

static void stack_push(QInstance *q, int key) {
    for (int i = 0; i < q->stack_n; i++)
        if (q->stack[i] == key) {
            memmove(&q->stack[i], &q->stack[i + 1], (size_t)(q->stack_n - i - 1) * sizeof(int));
            q->stack_n--;
            break;
        }
    if (q->stack_n >= Q_STACK_MAX) {
        memmove(&q->stack[0], &q->stack[1], (size_t)(Q_STACK_MAX - 1) * sizeof(int));
        q->stack_n--;
    }
    q->stack[q->stack_n++] = key;
}

static void stack_remove(QInstance *q, int key) {
    for (int i = 0; i < q->stack_n; i++)
        if (q->stack[i] == key) {
            memmove(&q->stack[i], &q->stack[i + 1], (size_t)(q->stack_n - i - 1) * sizeof(int));
            q->stack_n--;
            return;
        }
}

/* stereo slots for Mono Spread > 0, spread so neighbours land far apart */
static const float SPREAD_SLOT[8] = {-1.0f, 0.43f, -0.43f, 1.0f, -0.71f, 0.14f, 0.71f, -0.14f};

static void start(QInstance *q, QLineVoice *v, int key, float vel, int retrig, int glide) {
    float note = (float)key;
    int poly = (int)(q->pv[P_LN_MODE] + 0.5f) == 2;
    if (retrig || !v->active || key != v->key) {
        v->sp_rnd = q_randb(&q->rng);
        v->sp_slot = SPREAD_SLOT[(poly ? (uint32_t)(v - q->lv) : q->ln_step++) & 7];
        v->ncut = q_randb(&q->rng);   /* vintage: this note's cutoff and envelope tolerance */
        v->nenv = q_randb(&q->rng);
    }
    if (!v->active || !glide) v->pitch = note;
    v->target = note;
    v->key = key;
    v->vel = vel;
    v->gate = 1;
    v->sustained = 0;
    v->age = ++q->age;
    if (!v->active) {
        v->env = 0.0f;
        v->menv = 0.0f;
        v->y[0] = v->y[1] = v->y[2] = v->y[3] = 0.0f;
        v->p1 = 0.0f;
        v->p2 = q->pv[P_LN_SHAPE] >= (float)Q_LINE_VA ? 0.0f : 0.37f;
        v->wpos = -1.0f;
        v->drift = 0.0f;
        v->cdrift = 0.0f;
        v->pan = 2.0f;             /* snap to the first target */
        v->gl = v->gr = 0.0f;
    }
    if (retrig || !v->active) {
        v->st = 1;
        v->mst = 1;
        v->t_on = 0.0f;
    }
    v->active = 1;
}

void q_lines_note_on(QInstance *q, int key, float vel) {
    int mode = q_clampi((int)(q->pv[P_LN_MODE] + 0.5f), 0, 2);
    int glide = q->pv[P_LN_GLIDE] > 0.001f;
    if (mode == 2) {
        QLineVoice *best = 0;
        for (int i = 0; i < Q_MAX_LINES; i++)
            if (q->lv[i].active && q->lv[i].key == key) { best = &q->lv[i]; break; }
        if (!best)
            for (int i = 0; i < Q_MAX_LINES; i++)
                if (!q->lv[i].active) { best = &q->lv[i]; break; }
        if (!best) {
            uint32_t oldest = 0xFFFFFFFFu;
            for (int i = 0; i < Q_MAX_LINES; i++)
                if (q->lv[i].age < oldest) { oldest = q->lv[i].age; best = &q->lv[i]; }
        }
        start(q, best, key, vel, 1, 0);
        return;
    }
    int had = q->stack_n > 0 && q->lv[0].gate;
    stack_push(q, key);
    QLineVoice *v = &q->lv[0];
    start(q, v, key, vel, mode == 0 || !had, glide && v->active);
}

void q_lines_note_off(QInstance *q, int key) {
    int mode = q_clampi((int)(q->pv[P_LN_MODE] + 0.5f), 0, 2);
    if (mode == 2) {
        for (int i = 0; i < Q_MAX_LINES; i++) {
            QLineVoice *v = &q->lv[i];
            if (!v->active || !v->gate || v->key != key) continue;
            if (q->sustain) { v->sustained = 1; continue; }
            v->gate = 0;
            if (v->st == 2) v->st = 3;
        }
        return;
    }
    stack_remove(q, key);
    QLineVoice *v = &q->lv[0];
    if (!v->active || v->key != key) return;
    if (q->stack_n > 0) {
        int k = q->stack[q->stack_n - 1];
        v->key = k;
        v->target = (float)k;
        if (q->pv[P_LN_GLIDE] <= 0.001f) v->pitch = (float)k;
        return;
    }
    if (q->sustain) { v->sustained = 1; return; }
    v->gate = 0;
    if (v->st == 2) v->st = 3;
}

static inline float pulse(float p, float w, float dt) {
    float h = p + 1.0f - w;
    h -= (float)(int)h;
    return (p < w ? 1.0f : -1.0f) + q_blep(p, dt) - q_blep(h, dt) - (2.0f * w - 1.0f);
}

void q_lines_render(QInstance *q, float *outl, float *outr, int n) {
    const float *P = q->ps;
    float sr = q->sr;
    int shape = q_clampi((int)(P[P_LN_SHAPE] + 0.5f), 0, Q_NUM_LINE_SHAPES - 1);
    int wt = shape >= Q_LINE_VA ? shape - Q_LINE_VA : -1;
    QWave wv;
    int have = 0;
    if (wt >= 0) {
        q_wave_update(q, 1, wt);
        have = q_wave_get(q, 1, wt, &wv);
    }
    int sus_mode = P[P_LN_SUS] >= 0.5f;
    float var = P[P_LN_VAR], wenv = P[P_LN_WENV];
    float macro = wt < 0 ? var : 0.0f;
    float pwm_d = q_clamp(macro * 3.0f, 0.0f, 1.0f);
    float trem_d = q_clamp(macro * 3.0f - 1.0f, 0.0f, 1.0f);
    float vib_d = q_clamp(macro * 3.0f - 2.0f, 0.0f, 1.0f);
    float det = P[P_LN_DET] * 50.0f;
    float intv = (float)lroundf(P[P_LN_INT]);
    float oct = 12.0f * (float)q_clampi((int)lroundf(P[P_LN_OCT]), -2, 2);
    float res = P[P_LN_RES];
    float kres = 3.85f * q_clamp(res, 0.0f, 1.0f);
    float comp = 1.0f + kres * 0.4f;
    float a_t = 0.5f * q_env_time(P[P_LN_A]);
    float d_t = 0.005f + 0.9f * q_env_time(P[P_LN_D]);
    float ac_base = 1.0f - expf(-1.0f / (a_t / 1.609f * sr));
    float dc_base = 1.0f - expf(-1.0f / (d_t / 5.0f * sr));
    float dcm_base = 1.0f - expf(-1.0f / (0.66f * d_t / 5.0f * sr));   /* mod env: 66% of the decay */
    const float MOD_SUS = 0.5f;
    float glide_t = 1.2f * P[P_LN_GLIDE] * P[P_LN_GLIDE];
    float gc = glide_t > 0.0005f ? 1.0f - expf(-(float)Q_CTRL / (glide_t * 0.25f * sr)) : 1.0f;
    float vol = P[P_LN_VOL] * 0.42f;
    float velsens = P[P_VEL];
    float flav = P[P_VINTAGE];
    float spread = P[P_LN_SPREAD];
    float pan_c = 1.0f - expf(-1.0f / (0.004f * sr / (float)Q_CTRL)); /* ~4 ms pan glide */
    int ticks = (n + Q_CTRL - 1) / Q_CTRL;
    int wheel_vib = (int)(P[P_WHEEL] + 0.5f) == 0;
    float vib_hz = 3.0f + 6.0f * P[P_VIB_RATE];   /* shared with the Pads (default ~5.7 Hz) */
    float ext_vib = P[P_VIB] * 0.5f + q->at * 0.4f + (wheel_vib ? q->wheel * 0.6f : 0.0f);
    float dt_tick = (float)Q_CTRL / sr;

    for (int vi = 0; vi < Q_MAX_LINES; vi++) {
        QLineVoice *v = &q->lv[vi];
        if (!v->active) continue;
        /* vintage: each note's envelope runs up to +-25% faster or slower */
        float ek = 1.0f + 0.25f * flav * v->nenv;
        float ac = q_clamp(ac_base * ek, 0.0f, 1.0f), dc = q_clamp(dc_base * ek, 0.0f, 1.0f);
        /* the mod env runs at control rate: one tick = Q_CTRL samples of the same one-pole */
        float mac = 1.0f - powf(1.0f - ac, (float)Q_CTRL);
        float mdc = 1.0f - powf(1.0f - q_clamp(dcm_base * ek, 0.0f, 1.0f), (float)Q_CTRL);
        float vs = 1.0f - velsens * (1.0f - v->vel);              /* velocity -> env amounts */
        float vg = vol * expf(-1.036f * (1.0f - v->vel));          /* velocity -> level, 9 dB */
        float pwm_ph = q->lfo_ln_pwm, trem_ph = q->lfo_ln_trem, vib_ph = q->lfo_ln_vib;
        for (int t = 0; t < ticks; t++) {
            int i0 = t * Q_CTRL;
            int m = n - i0 < Q_CTRL ? n - i0 : Q_CTRL;
            pwm_ph += 0.6f * dt_tick; pwm_ph -= floorf(pwm_ph);
            trem_ph += 4.8f * dt_tick; trem_ph -= floorf(trem_ph);
            vib_ph += vib_hz * dt_tick; vib_ph -= floorf(vib_ph);
            float s_pwm = sinf(Q_TAU * pwm_ph), s_trem = sinf(Q_TAU * trem_ph), s_vib = sinf(Q_TAU * vib_ph);
            v->t_on += dt_tick;
            if (v->mst == 1) {
                v->menv += (1.25f - v->menv) * mac;
                if (v->menv >= 1.0f) { v->menv = 1.0f; v->mst = 2; }
            } else {
                if (v->mst == 2 && v->st == 3) v->mst = 3;            /* key released (Sustain mode) */
                float tgt = v->mst == 2 && sus_mode ? MOD_SUS : 0.0f;
                v->menv += (tgt - v->menv) * mdc;
            }
            v->pitch += (v->target - v->pitch) * gc;
            v->drift += q_randb(&q->rng) * 0.05f - v->drift * 0.002f;
            v->drift = q_clamp(v->drift, -1.0f, 1.0f);
            v->cdrift += q_randb(&q->rng) * 0.05f - v->cdrift * 0.002f;
            v->cdrift = q_clamp(v->cdrift, -1.0f, 1.0f);
            float onset = q_clamp(v->t_on * 3.0f - 0.4f, 0.0f, 1.0f);
            float pitch = v->pitch + oct + q->bend * 2.0f + flav * 0.06f * v->drift +
                          (vib_d * 0.32f * onset + ext_vib) * s_vib;
            float hz1 = q_note_hz(pitch);
            float cents2 = det + (shape != 1 ? pwm_d * 7.0f * s_pwm : 0.0f);
            float hz2 = q_note_hz(pitch + intv + cents2 * 0.01f);
            if (wt >= 0 && have) { hz1 /= wv.div; hz2 /= wv.div; }   /* chord stacks: table cycle = DIV periods */
            v->inc1 = hz1 / sr;
            v->inc2 = q_clamp(hz2 / sr, 0.0f, 0.45f);
            v->pw = 0.5f + 0.42f * pwm_d * s_pwm;
            float cn = 20.0f + 115.0f * P[P_LN_CUT] + 0.5f * (v->pitch - 60.0f) +
                       P[P_LN_FENV] * 72.0f * vs * v->menv + velsens * 18.0f * (v->vel - 0.6f) +
                       flav * (1.2f * v->ncut + 0.8f * v->cdrift); /* vintage: +-2 st at most */
            float fc = q_clamp(q_note_hz(cn), 16.0f, 0.33f * sr);
            v->cg = 1.0f - expf(-Q_TAU * fc / sr);
            int m1 = 0, m2 = 0;
            float dwp = 0.0f;
            if (wt >= 0 && have) {
                /* Wave Env scales to the room left in the table, so Variation + Wave Env never
                 * runs past either end (full Wave Env reaches exactly the last / first wave) */
                float we = wenv * vs * (v->menv < 1.0f ? v->menv : 1.0f);
                float pos = q_clamp(var + we * (we >= 0.0f ? 1.0f - var : var), 0.0f, 1.0f);
                if (v->wpos < 0.0f) v->wpos = pos;
                dwp = (pos - v->wpos) / (float)m;
                m1 = q_wave_mip(&wv, hz1, sr);
                m2 = q_wave_mip(&wv, hz2, sr);
            }
            float ptg = spread < 0.0f ? -spread * v->sp_rnd : spread * v->sp_slot;
            v->pan = v->pan > 1.5f ? ptg : v->pan + (ptg - v->pan) * pan_c;
            float pa = (q_clamp(v->pan, -1.0f, 1.0f) + 1.0f) * (Q_PI * 0.25f);
            float tgl = vg * cosf(pa) * 1.4142f * (1.0f - trem_d * 0.6f * (0.5f + 0.5f * s_trem));
            float tgr = vg * sinf(pa) * 1.4142f * (1.0f - trem_d * 0.6f * (0.5f - 0.5f * s_trem));
            float gl = v->gl, gr = v->gr, dgl = (tgl - gl) / (float)m, dgr = (tgr - gr) / (float)m;

            float p1 = v->p1, p2 = v->p2, env = v->env, g = v->cg, w = v->pw;
            float y0 = v->y[0], y1 = v->y[1], y2 = v->y[2], y3 = v->y[3];
            int st = v->st;
            float d1 = v->inc1, d2 = v->inc2, wp = v->wpos;
            for (int i = 0; i < m; i++) {
                float x;
                p1 += d1;
                int wrapped = 0;
                if (p1 >= 1.0f) { p1 -= 1.0f; wrapped = 1; }
                if (wt >= 0) {
                    p2 += d2;
                    if (p2 >= 1.0f) p2 -= 1.0f;
                    x = have ? 0.55f * (q_wave_read(&wv, m1, wp, p1) + q_wave_read(&wv, m2, wp, p2)) : 0.0f;
                    wp += dwp;
                } else if (shape == 3) {
                    p2 += d2;
                    if (wrapped) {
                        float before = 2.0f * (p2 - (float)(int)p2) - 1.0f;
                        p2 = p1 * (d2 / d1);
                        float after = 2.0f * p2 - 1.0f;
                        x = after + 0.5f * (after - before) * q_blep(p1, d1);
                    } else {
                        if (p2 >= 1.0f) p2 -= 1.0f;
                        x = 2.0f * p2 - 1.0f - q_blep(p2, d2);
                    }
                    x = 0.85f * x + 0.25f * (2.0f * p1 - 1.0f - q_blep(p1, d1));
                } else {
                    p2 += d2;
                    if (p2 >= 1.0f) p2 -= 1.0f;
                    float a = shape == 1 ? pulse(p1, w, d1) : 2.0f * p1 - 1.0f - q_blep(p1, d1);
                    float b = shape == 0 ? 2.0f * p2 - 1.0f - q_blep(p2, d2) : pulse(p2, w, d2);
                    x = 0.55f * (a + b);
                }
                /* envelope */
                if (st == 1) {
                    env += (1.25f - env) * ac;
                    if (env >= 1.0f) { env = 1.0f; st = sus_mode ? 2 : 3; }
                } else if (st == 3) {
                    env -= env * dc;
                } else if (st == 2 && !sus_mode) {
                    st = 3;
                }
                /* ladder: tanh on the input, unit-delay feedback */
                float in = q_tanh(1.2f * x - kres * y3);
                y0 += (in - y0) * g;
                y1 += (y0 - y1) * g;
                y2 += (y1 - y2) * g;
                y3 += (y2 - y3) * g;
                float s = y3 * comp * env;
                gl += dgl;
                gr += dgr;
                outl[i0 + i] += s * gl;
                outr[i0 + i] += s * gr;
            }
            v->p1 = p1; v->p2 = p2; v->env = env; v->st = st; v->gl = gl; v->gr = gr;
            if (wt >= 0) v->wpos = wp;
            v->y[0] = y0; v->y[1] = y1; v->y[2] = y2; v->y[3] = y3;
            if (st == 3 && env < 0.0002f) {
                v->active = 0;
                v->gate = 0;
                break;
            }
        }
    }
    float adv = (float)n / sr;
    q->lfo_ln_pwm += 0.6f * adv; q->lfo_ln_pwm -= floorf(q->lfo_ln_pwm);
    q->lfo_ln_trem += 4.8f * adv; q->lfo_ln_trem -= floorf(q->lfo_ln_trem);
    q->lfo_ln_vib += vib_hz * adv; q->lfo_ln_vib -= floorf(q->lfo_ln_vib);
}
