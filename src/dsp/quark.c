/* Schwung v2 entry point, parameters / presets / state, MIDI routing, arpeggiator and the mix. */
#include "quark.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef QUARK_PROFILE
#include <time.h>
#endif

#include "tables_gen.h"

static QInstance g_pool[Q_NUM_SLOTS];
static const host_api_v1_t *g_host;

/* ------------------------------------------------------------------ params */

static int find_param(const char *key) {
    char c0 = key[0];
    for (int i = 0; i < P_COUNT; i++) {
        const char *k = quark_params[i].key;
        if (k[0] == c0 && strcmp(k, key) == 0) return i;
    }
    return -1;
}

static float sanitize(int i, float v) {
    const QParam *p = &quark_params[i];
    if (!(v == v)) v = p->def;
    v = q_clamp(v, p->mn, p->mx);
    if (p->kind != 'f') v = floorf(v + 0.5f);
    return v;
}

static void set_value(QInstance *q, int i, float v) {
    v = sanitize(i, v);
    q->pv[i] = v;
    /* steps snap; continuous values glide (smoothed once per block) */
    if (quark_params[i].kind != 'f') q->ps[i] = v;
}

static void apply_preset(QInstance *q, int idx) {
    idx = q_clampi(idx, 0, quark_preset_count - 1);
    q->preset = idx;
    for (int i = 0; i < P_COUNT; i++) {
        set_value(q, i, quark_presets[idx][i]);
        q->ps[i] = q->pv[i];
    }
}

static void param_set_str(QInstance *q, int i, const char *val) {
    const QParam *p = &quark_params[i];
    char *end = 0;
    float v = strtof(val, &end);
    if (end == val) {
        if (p->kind != 'e') return;
        for (int k = 0; k < p->nopts; k++)
            if (strcmp(p->opts[k], val) == 0) { set_value(q, i, (float)k); return; }
        return;
    }
    set_value(q, i, v);
}

static int fmt_value(QInstance *q, int i, char *buf, int len) {
    const QParam *p = &quark_params[i];
    int n;
    if (p->kind == 'f') n = snprintf(buf, (size_t)len, "%.4f", (double)q->pv[i]);
    else n = snprintf(buf, (size_t)len, "%d", (int)q->pv[i]);
    return (n < 0 || n >= len) ? -1 : n;
}

static int copy_out(const char *s, char *buf, int len) {
    int n = (int)strlen(s);
    if (n >= len) return -1;
    memcpy(buf, s, (size_t)n + 1);
    return n;
}

/* state: {"v":1,"preset":3,"p":{"tx_type":0,...}} */
static int state_get(QInstance *q, char *buf, int len) {
    int pos = snprintf(buf, (size_t)len, "{\"v\":1,\"preset\":%d,\"p\":{", q->preset);
    if (pos < 0 || pos >= len) return -1;
    for (int i = 0; i < P_COUNT; i++) {
        char tmp[48];
        fmt_value(q, i, tmp, sizeof tmp);
        int n = snprintf(buf + pos, (size_t)(len - pos), "%s\"%s\":%s", i ? "," : "", quark_params[i].key, tmp);
        if (n < 0 || n >= len - pos) return -1;
        pos += n;
    }
    int n = snprintf(buf + pos, (size_t)(len - pos), "}}");
    if (n < 0 || n >= len - pos) return -1;
    return pos + n;
}

static void state_set(QInstance *q, const char *js) {
    /* start from the init patch so a state that omits keys is still complete */
    for (int i = 0; i < P_COUNT; i++) set_value(q, i, quark_params[i].def);
    const char *pp = strstr(js, "\"preset\"");
    if (pp) {
        pp = strchr(pp, ':');
        if (pp) q->preset = q_clampi(atoi(pp + 1), 0, quark_preset_count - 1);
    }
    const char *s = strstr(js, "\"p\"");
    s = s ? strchr(s, '{') : js;
    if (!s) return;
    while (*s) {
        const char *k0 = strchr(s, '"');
        if (!k0) break;
        const char *k1 = strchr(k0 + 1, '"');
        if (!k1) break;
        char key[40];
        int kl = (int)(k1 - k0 - 1);
        s = k1 + 1;
        if (kl <= 0 || kl >= (int)sizeof key) continue;
        memcpy(key, k0 + 1, (size_t)kl);
        key[kl] = 0;
        while (*s == ' ' || *s == ':') s++;
        int i = find_param(key);
        if (i >= 0) {
            char *end = 0;
            float v = strtof(s, &end);
            if (end != s) set_value(q, i, v);
            s = end ? end : s;
        }
    }
    for (int i = 0; i < P_COUNT; i++) q->ps[i] = q->pv[i];
}

/* ------------------------------------------------------------------ note routing */

/* layers a key plays: bit 0 texture, bit 1 lines */
static int key_layers(QInstance *q, int key) {
    int mode = q_clampi((int)(q->pv[P_KEY_MODE] + 0.5f), 0, 3);
    int m;
    switch (mode) {
    case 1: m = key < (int)q->pv[P_SPLIT] ? 2 : 1; break;
    case 2: m = 1; break;
    case 3: m = 2; break;
    default: m = 3; break;
    }
    if (q->pv[P_TX_VOL] < 0.001f) m &= ~1;
    if (q->pv[P_LN_VOL] < 0.001f) m &= ~2;
    return m;
}

static int arp_on(QInstance *q) { return q->pv[P_ARP_MODE] >= 0.5f; }

/* ---- pads: latch + the chord the arp follows ---- */
static void pad_release_latched(QInstance *q) {
    for (int k = 0; k < 128; k++)
        if (q->pad_on[k] && !q->pad_down[k]) { q->pad_on[k] = 0; q_tex_note_off(q, k); }
}

static void pad_note_on(QInstance *q, int key, float vel) {
    /* Latch: notes hold after release; the first key of a new chord clears the old one */
    if (q->pv[P_TX_LATCH] >= 0.5f && q->pad_down_n == 0) pad_release_latched(q);
    if (!q->pad_down[key]) { q->pad_down[key] = 1; q->pad_down_n++; }
    q->pad_on[key] = 1;
    q->pad_ord[key] = ++q->ord;
    q_tex_note_on(q, key, key, vel);
}

static void pad_note_off(QInstance *q, int key) {
    if (q->pad_down[key]) { q->pad_down[key] = 0; q->pad_down_n--; }
    if (q->pad_on[key] && q->pv[P_TX_LATCH] >= 0.5f) return;
    if (!q->sustain) q->pad_on[key] = 0;   /* sustained notes leave the chord on pedal up */
    q_tex_note_off(q, key);
}

/* ---- the Mono arp ---- */
static void arp_silence(QInstance *q) {
    QArp *a = &q->arp;
    for (int i = 0; i < a->on_count; i++) q_lines_note_off(q, a->on_note[i]);
    a->on_count = 0;
}

static void arp_key_add(QInstance *q, int key) {
    QArp *a = &q->arp;
    for (int i = 0; i < a->count; i++)
        if (a->note[i] == key) return;
    if (a->count >= Q_ARP_MAX) return;
    a->note[a->count] = key;
    a->nord[a->count++] = ++q->ord;
}

static void arp_key_remove(QInstance *q, int key) {
    QArp *a = &q->arp;
    for (int i = 0; i < a->count; i++)
        if (a->note[i] == key) {
            memmove(&a->note[i], &a->note[i + 1], (size_t)(a->count - i - 1) * sizeof(int));
            memmove(&a->nord[i], &a->nord[i + 1], (size_t)(a->count - i - 1) * sizeof(uint32_t));
            a->count--;
            return;
        }
}

/* The notes the arp plays, in press order: the chord sounding on the Pads (Source = Pads Chord,
 * so it follows chords as they change, latched ones included) plus any keys routed to Mono. */
static int arp_live(QInstance *q, int *out) {
    QArp *a = &q->arp;
    uint32_t ord[Q_ARP_MAX];
    int n = 0;
    if (q->pv[P_ARP_SRC] >= 0.5f)
        for (int k = 0; k < 128 && n < Q_ARP_MAX; k++)
            if (q->pad_on[k]) { out[n] = k; ord[n++] = q->pad_ord[k]; }
    for (int i = 0; i < a->count && n < Q_ARP_MAX; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++) dup |= out[j] == a->note[i];
        if (!dup) { out[n] = a->note[i]; ord[n++] = a->nord[i]; }
    }
    for (int i = 1; i < n; i++) {
        int x = out[i]; uint32_t o = ord[i]; int j = i - 1;
        while (j >= 0 && ord[j] > o) { out[j + 1] = out[j]; ord[j + 1] = ord[j]; j--; }
        out[j + 1] = x; ord[j + 1] = o;
    }
    return n;
}

/* With Latch on, an empty pool replays the last chord (the set as it stood after its last note-on)
 * until a new chord starts. */
static int arp_pool(QInstance *q, int *out) {
    QArp *a = &q->arp;
    int n = arp_live(q, out);
    if (n == 0 && q->pv[P_ARP_HOLD] >= 0.5f && a->snap_n > 0) {
        n = a->snap_n;
        memcpy(out, a->snap, (size_t)n * sizeof(int));
    }
    return n;
}

static void arp_snapshot(QInstance *q) {
    int tmp[Q_ARP_MAX], n = arp_live(q, tmp);
    if (n > 0) {
        memcpy(q->arp.snap, tmp, (size_t)n * sizeof(int));
        q->arp.snap_n = n;
    }
}

static void note_on(QInstance *q, int key, float vel) {
    int m = key_layers(q, key);
    q->last_vel = vel;
    if (m & 1) pad_note_on(q, key, vel);
    if (m & 2) {
        if (arp_on(q)) arp_key_add(q, key);
        else q_lines_note_on(q, key, vel);
    }
    if (arp_on(q)) arp_snapshot(q);
}

static void note_off(QInstance *q, int key) {
    /* release on every layer regardless of the current routing, so a mode change never hangs */
    arp_key_remove(q, key);
    if (!arp_on(q)) q_lines_note_off(q, key);
    pad_note_off(q, key);
}

/* ---- arp variation dice: deterministic per clock step, so Loop > 0 repeats the same variation */
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static float arp_dice(const QArp *a, int loop, uint32_t salt) {
    uint32_t st = loop > 0 ? a->mstep % (uint32_t)loop : a->mstep;
    return (float)(mix32(st * 0x9E3779B9u ^ mix32(salt)) & 0xFFFFFF) * (1.0f / 16777216.0f);
}

static int leap_in(int step, int count) {
    step %= count;
    return (step & 1) == 0 ? step / 2 : count - 1 - step / 2;
}
static int leap_out(int step, int count) {
    step %= count;
    int idx;
    if (count & 1) {
        int c = count / 2, k = (step + 1) / 2;
        idx = step == 0 ? c : c + ((step & 1) ? -k : k);
    } else {
        int lo = count / 2 - 1, hi = count / 2;
        idx = step == 0 ? lo : (step & 1) ? hi + (step - 1) / 2 : lo - step / 2;
    }
    return q_clampi(idx, 0, count - 1);
}
/* a pattern asks for held note i (1-based) beyond the chord: fold back down (1 2 3 2 1 ...) */
static int fold_index(int i, int held) {
    if (i <= held) return i;
    if (held == 1) return 1;
    int period = 2 * held - 2, k = ((i - 1) % period) + 1;
    return k <= held ? k : period - k + 2;
}

static void arp_release_due(QInstance *q, double beat_now) {
    QArp *a = &q->arp;
    for (int i = 0; i < a->on_count;) {
        if (beat_now >= a->on_off[i]) {
            q_lines_note_off(q, a->on_note[i]);
            a->on_note[i] = a->on_note[a->on_count - 1];
            a->on_off[i] = a->on_off[a->on_count - 1];
            a->on_count--;
        } else {
            i++;
        }
    }
}

static void arp_play(QInstance *q, int key, float vel, double off) {
    QArp *a = &q->arp;
    for (int i = 0; i < a->on_count; i++)   /* the same key again: end the old one first */
        if (a->on_note[i] == key) {
            q_lines_note_off(q, key);
            a->on_note[i] = a->on_note[--a->on_count];
            a->on_off[i] = a->on_off[a->on_count];
            break;
        }
    if (a->on_count >= Q_ARP_MAX) {
        q_lines_note_off(q, a->on_note[0]);
        a->on_note[0] = a->on_note[--a->on_count];
        a->on_off[0] = a->on_off[a->on_count];
    }
    q_lines_note_on(q, key, vel);
    a->on_note[a->on_count] = key;
    a->on_off[a->on_count++] = off;
}

#define Q_ARP_EXT (Q_ARP_MAX * 5)

static void arp_tick(QInstance *q, double beat_now) {
    QArp *a = &q->arp;
    const float *P = q->pv;
    arp_release_due(q, beat_now);
    int notes[Q_ARP_MAX];
    int n = arp_pool(q, notes);
    if (n == 0) return;
    int ri = q_clampi((int)(P[P_ARP_RATE] + 0.5f), 0, 6);
    double len = Q_ARP_BEATS[ri];
    double swing = 0.33 * P[P_ARP_SWING];
    long step = (long)floor(beat_now / len);
    /* swing: odd steps start late */
    if (step & 1) {
        double into = beat_now - (double)step * len;
        if (into < swing * len) step--;
    }
    if (step == a->step) return;
    a->step = step;
    a->mstep++;
    int rh = q_clampi((int)(P[P_ARP_RHY] + 0.5f), 0, Q_NUM_ARP_RHYTHMS - 1);
    int rlen = Q_ARP_RHY[rh][1];
    if (!((Q_ARP_RHY[rh][0] >> (int)(((step % rlen) + rlen) % rlen)) & 1)) return;
    int loop = q_clampi((int)(P[P_ARP_LOOP] + 0.5f), 0, 32);
    if (P[P_ARP_DROP] > 0.0f && arp_dice(a, loop, 0xD0A4u) < P[P_ARP_DROP]) return;

    int mode = q_clampi((int)(P[P_ARP_MODE] + 0.5f), 1, 10);
    const signed char *os = Q_ARP_OCT[q_clampi((int)(P[P_ARP_OCT] + 0.5f), 0, Q_NUM_ARP_OCTS - 1)];
    int ol = os[0];
    if (mode != 6) /* every type but Order works on the notes by pitch */
        for (int i = 1; i < n; i++) {
            int x = notes[i], j = i - 1;
            while (j >= 0 && notes[j] > x) { notes[j + 1] = notes[j]; j--; }
            notes[j + 1] = x;
        }

    int out[Q_ARP_MAX], cnt = 0;
    if (mode == 7 || mode == 10) {
        /* Chord / Pattern: whole steps; the octave moves on each pass through the pattern */
        int plen = 1;
        if (mode == 7) {
            for (int i = 0; i < n; i++) out[cnt++] = notes[i];
        } else {
            const unsigned char *pt = Q_ARP_PAT[q_clampi((int)(P[P_ARP_PAT] + 0.5f), 0, Q_NUM_ARP_PATTERNS - 1)];
            plen = pt[0];
            a->pos = (a->pos + 1) % plen;
            int mask = pt[1 + a->pos];
            for (int b = 0; b < 5; b++) {
                if (!(mask & (1 << b))) continue;
                int k = notes[fold_index(b + 1, n) - 1], dup = 0;
                for (int j = 0; j < cnt; j++) dup |= out[j] == k;
                if (!dup) out[cnt++] = k;
            }
        }
        int oc = os[1 + (int)((a->emit / plen) % ol)];
        for (int i = 0; i < cnt; i++) out[i] += 12 * oc;
    } else {
        /* the other types walk the held notes stretched over the octave range */
        int ext[Q_ARP_EXT], total = 0;
        if (mode == 6) {
            for (int o = 0; o < ol; o++)
                for (int i = 0; i < n && total < Q_ARP_EXT; i++) ext[total++] = notes[i] + 12 * os[1 + o];
        } else {
            int lo = os[1], hi = os[1];
            for (int o = 1; o < ol; o++) { if (os[1 + o] < lo) lo = os[1 + o]; if (os[1 + o] > hi) hi = os[1 + o]; }
            for (int o = lo; o <= hi; o++)
                for (int i = 0; i < n && total < Q_ARP_EXT; i++) ext[total++] = notes[i] + 12 * o;
        }
        int idx;
        if (mode == 5) {
            idx = (int)(arp_dice(a, loop, 0x5EEDu) * (float)total);
        } else if ((mode == 3 || mode == 4) && total > 1) {
            a->pos += a->dir;
            if (a->pos >= total) { a->pos = total - 2; a->dir = -1; }
            if (a->pos < 0) { a->pos = 1; a->dir = 1; }
            idx = mode == 4 ? total - 1 - a->pos : a->pos;
        } else {
            a->pos = (a->pos + 1) % total;
            idx = mode == 2 ? total - 1 - a->pos : mode == 8 ? leap_in(a->pos, total) : mode == 9 ? leap_out(a->pos, total) : a->pos;
        }
        out[cnt++] = ext[q_clampi(idx, 0, total - 1)];
    }
    a->emit++;

    /* variation: note swap, octave jump, velocity, gate */
    float nr = P[P_ARP_NRND], orn = P[P_ARP_ORND];
    for (int i = 0; i < cnt; i++) {
        if (nr > 0.0f && n > 1 && arp_dice(a, loop, 0x91E3u + (uint32_t)i * 131u) < nr) {
            int base = out[i], oc = 0;
            while (base - 12 * oc > notes[n - 1]) oc++;
            while (base - 12 * oc < notes[0]) oc--;
            int k = (int)(arp_dice(a, loop, 0x7B1Du + (uint32_t)i) * (float)(n - 1)) + 1;
            int self = 0;
            for (int j = 0; j < n; j++) if (notes[j] == base - 12 * oc) self = j;
            out[i] = notes[(self + k) % n] + 12 * oc;
        }
        if (orn > 0.0f && arp_dice(a, loop, 0x7F1Du + (uint32_t)i) < orn)
            out[i] += arp_dice(a, loop, 0x0C7Au + (uint32_t)i) < 0.5f ? -12 : 12;
        out[i] = q_clampi(out[i], 0, 127);
    }
    int fixed = (int)(P[P_ARP_VEL] + 0.5f);
    float vel = fixed > 0 ? (float)fixed / 127.0f : (q->last_vel > 0.0f ? q->last_vel : 0.8f);
    if (P[P_ARP_VRND] > 0.0f) vel += (arp_dice(a, loop, 0xA11CEu) - 0.5f) * P[P_ARP_VRND];
    vel = q_clamp(vel, 0.05f, 1.0f);
    float gx = P[P_ARP_GATE], g = 0.05f + 1.95f * gx * gx;
    if (P[P_ARP_GRND] > 0.0f) g *= 1.0f + (arp_dice(a, loop, 0x6A73u) - 0.5f) * 1.6f * P[P_ARP_GRND];
    if (g < 0.03f) g = 0.03f;
    double off = beat_now + len * (double)g;
    for (int i = 0; i < cnt; i++) arp_play(q, out[i], vel, off);
}

static void arp_reset(QInstance *q) {
    arp_silence(q);
    q->arp.count = q->arp.snap_n = 0;
    q->arp.running = 0;
}

void q_release_all(QInstance *q, int hard) {
    arp_reset(q);
    q->stack_n = 0;
    q->sustain = 0;
    memset(q->pad_down, 0, sizeof q->pad_down);
    memset(q->pad_on, 0, sizeof q->pad_on);
    q->pad_down_n = 0;
    for (int i = 0; i < Q_MAX_TEX; i++) {
        QTexVoice *v = &q->tv[i];
        if (hard) { v->active = 0; continue; }
        if (v->active) { v->gate = 0; v->amp_st = 3; v->mod_st = 3; v->kill = 0; }
    }
    for (int i = 0; i < Q_MAX_LINES; i++) {
        QLineVoice *v = &q->lv[i];
        if (hard) { v->active = 0; continue; }
        if (v->active) { v->gate = 0; v->st = 3; }
    }
}

static void sustain_release(QInstance *q) {
    if (q->pv[P_TX_LATCH] < 0.5f)
        for (int k = 0; k < 128; k++)
            if (q->pad_on[k] && !q->pad_down[k]) q->pad_on[k] = 0;
    for (int i = 0; i < Q_MAX_TEX; i++) {
        QTexVoice *v = &q->tv[i];
        if (v->active && v->sustained) { v->sustained = 0; v->gate = 0; v->amp_st = 3; v->mod_st = 3; }
    }
    for (int i = 0; i < Q_MAX_LINES; i++) {
        QLineVoice *v = &q->lv[i];
        if (v->active && v->sustained) { v->sustained = 0; v->gate = 0; if (v->st == 2) v->st = 3; }
    }
}

/* ------------------------------------------------------------------ plugin */

static void *q_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;
    q_tables_init();
    QInstance *q = 0;
    for (int i = 0; i < Q_NUM_SLOTS; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_pool[i].used, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            q = &g_pool[i];
            break;
        }
    }
    if (!q) return 0;
    memset(q, 0, sizeof *q);
    q->used = 1;
    q->host = g_host;
    q->sr = (g_host && g_host->sample_rate > 0) ? (float)g_host->sample_rate : 44100.0f;
    q->rng = 0x9E3779B9u ^ (uint32_t)(uintptr_t)q;
    q->bpm = 120.0f;
    q->sm_vol = -1.0f;
    for (int i = 0; i < 2; i++) { q->am[i].have[0] = q->am[i].have[1] = -1; q->am[i].build_id = -1; }
    q_fx_init(q);
    q_fx_clear(q);
    apply_preset(q, 0);
    return q;
}

static void q_destroy(void *inst) {
    QInstance *q = (QInstance *)inst;
    if (!q) return;
    __atomic_store_n(&q->used, 0, __ATOMIC_RELEASE);
}

static void q_midi(void *inst, const uint8_t *msg, int len, int source) {
    (void)source;
    QInstance *q = (QInstance *)inst;
    if (!q || !msg || len < 1) return;
    int st = msg[0] & 0xF0;
    switch (st) {
    case 0x90:
        if (len < 3) return;
        if (msg[2]) note_on(q, msg[1], (float)msg[2] / 127.0f);
        else note_off(q, msg[1]);
        break;
    case 0x80:
        if (len < 3) return;
        note_off(q, msg[1]);
        break;
    case 0xB0:
        if (len < 3) return;
        if (msg[1] == 1) q->wheel = (float)msg[2] / 127.0f;
        else if (msg[1] == 64) {
            int on = msg[2] >= 64;
            if (q->sustain && !on) { q->sustain = 0; sustain_release(q); }
            q->sustain = on;
        } else if (msg[1] == 120 || msg[1] == 123) q_release_all(q, msg[1] == 120);
        else if (msg[1] == 121) { q->wheel = 0.0f; q->bend = 0.0f; q->at = 0.0f; }
        break;
    case 0xD0:
        if (len < 2) return;
        q->at = (float)msg[1] / 127.0f;
        break;
    case 0xE0:
        if (len < 3) return;
        q->bend = (float)(((int)msg[2] << 7 | msg[1]) - 8192) / 8192.0f;
        break;
    default: break;
    }
}

static void q_set_param(void *inst, const char *key, const char *val) {
    QInstance *q = (QInstance *)inst;
    if (!q || !key || !val) return;
    if (strcmp(key, "state") == 0) { state_set(q, val); return; }
    if (strcmp(key, "preset") == 0) { apply_preset(q, atoi(val)); return; }
    if (strcmp(key, "all_notes_off") == 0) { q_release_all(q, 0); return; }
    int i = find_param(key);
    if (i < 0) return;
    int was_arp = arp_on(q), was_latch = q->pv[P_TX_LATCH] >= 0.5f;
    param_set_str(q, i, val);
    if (i == P_ARP_MODE && was_arp != arp_on(q)) {
        arp_reset(q);
        /* Mono notes played directly before the arp took over (or vice versa) end here */
        for (int k = 0; k < Q_MAX_LINES; k++)
            if (q->lv[k].active) { q->lv[k].gate = 0; q->lv[k].st = 3; }
        q->stack_n = 0;
    }
    if (i == P_TX_LATCH && was_latch && q->pv[P_TX_LATCH] < 0.5f) pad_release_latched(q);
}

static int q_get_param(void *inst, const char *key, char *buf, int len) {
    QInstance *q = (QInstance *)inst;
    if (!q || !key || !buf || len <= 0) return -1;
    if (strcmp(key, "chain_params") == 0) return copy_out(quark_chain_params, buf, len);
    if (strcmp(key, "ui_hierarchy") == 0) return copy_out(quark_ui_hierarchy, buf, len);
    if (strcmp(key, "state") == 0) return state_get(q, buf, len);
    if (strcmp(key, "preset") == 0) { char t[16]; snprintf(t, sizeof t, "%d", q->preset); return copy_out(t, buf, len); }
    if (strcmp(key, "preset_count") == 0) { char t[16]; snprintf(t, sizeof t, "%d", quark_preset_count); return copy_out(t, buf, len); }
    if (strcmp(key, "preset_name") == 0) return copy_out(quark_preset_names[q->preset], buf, len);
    if (strcmp(key, "tx_var:effective") == 0) {
        char t[24]; snprintf(t, sizeof t, "%.4f", (double)q->var_eff); return copy_out(t, buf, len);
    }
    if (strcmp(key, "flt_cut:effective") == 0) {
        char t[24]; snprintf(t, sizeof t, "%.4f", (double)q_clamp((q->cut_eff - 20.0f) / 115.0f, 0.0f, 1.0f));
        return copy_out(t, buf, len);
    }
    if (strcmp(key, "arp_notes") == 0) { /* the arp's current note pool, comma separated (UI + tests) */
        int nt[Q_ARP_MAX], nn = arp_on(q) ? arp_pool(q, nt) : 0, pos = 0;
        char t[Q_ARP_MAX * 4 + 2];
        t[0] = 0;
        for (int k = 0; k < nn; k++) pos += snprintf(t + pos, sizeof t - (size_t)pos, k ? ",%d" : "%d", nt[k]);
        return copy_out(t, buf, len);
    }
    if (strcmp(key, "arp_playing") == 0) { /* notes the arp is sounding right now */
        char t[Q_ARP_MAX * 4 + 2];
        int pos = 0;
        t[0] = 0;
        for (int k = 0; k < q->arp.on_count; k++)
            pos += snprintf(t + pos, sizeof t - (size_t)pos, k ? ",%d" : "%d", q->arp.on_note[k]);
        return copy_out(t, buf, len);
    }
    if (strcmp(key, "voices") == 0) {
        int a = 0, b = 0;
        for (int i = 0; i < Q_MAX_TEX; i++) a += q->tv[i].active;
        for (int i = 0; i < Q_MAX_LINES; i++) b += q->lv[i].active;
        char t[24]; snprintf(t, sizeof t, "%d,%d", a, b); return copy_out(t, buf, len);
    }
#ifdef QUARK_PROFILE
    if (strcmp(key, "cpu_us") == 0) { char t[24]; snprintf(t, sizeof t, "%.2f", (double)q->cpu_us); return copy_out(t, buf, len); }
#endif
    size_t kl = strlen(key);
    if (kl > 5 && strcmp(key + kl - 5, ":base") == 0) {
        char k[40];
        if (kl - 5 >= sizeof k) return -1;
        memcpy(k, key, kl - 5);
        k[kl - 5] = 0;
        int i = find_param(k);
        return i < 0 ? -1 : fmt_value(q, i, buf, len);
    }
    int i = find_param(key);
    if (i < 0) return -1;
    return fmt_value(q, i, buf, len);
}

static int q_get_error(void *inst, char *buf, int len) {
    (void)inst;
    if (buf && len > 0) buf[0] = 0;
    return 0;
}

static void env_coefs(QInstance *q) {
    const float *P = q->ps;
    float sr = q->sr;
    q->tx_ac = 1.0f - expf(-1.0f / (q_env_time(P[P_TX_A]) / 1.609f * sr));
    q->tx_dc = 1.0f - expf(-1.0f / (q_env_time(P[P_TX_D]) / 5.0f * sr));
    q->tx_rc = 1.0f - expf(-1.0f / (q_env_time(P[P_TX_R]) / 5.0f * sr));
    q->me_ac = 1.0f - expf(-1.0f / (q_env_time(P[P_ME_A]) / 1.609f * sr));
    q->me_dc = 1.0f - expf(-1.0f / (q_env_time(P[P_ME_D]) / 5.0f * sr));
    q->me_rc = 1.0f - expf(-1.0f / (q_env_time(P[P_ME_R]) / 5.0f * sr));
}

static void render_chunk(QInstance *q, int16_t *out, int n) {
    /* smooth continuous params once per chunk (~3 ms) */
    for (int i = 0; i < P_COUNT; i++)
        if (quark_params[i].kind == 'f') q->ps[i] += (q->pv[i] - q->ps[i]) * 0.3f;
    env_coefs(q);

    /* arp clock: host beat while the transport runs, else our own (restarted with each new chord,
     * so the first note lands on the key press) */
    QArp *ar = &q->arp;
    if (arp_on(q)) {
        int tmp[Q_ARP_MAX];
        int pn = arp_pool(q, tmp);
        if (pn > 0 && !ar->running) {
            ar->running = 1;
            ar->step = -1;
            ar->pos = -1;
            ar->dir = 1;
            ar->beat = 0.0;
            ar->emit = 0;
            ar->mstep = 0;
        } else if (pn == 0 && ar->running) {
            arp_silence(q);
            ar->running = 0;
        }
    }
    if (ar->running && arp_on(q)) {
        double bpf = (double)q->bpm / (60.0 * (double)q->sr);
        double beat = q->transport && q->beat >= 0.0 ? q->beat : ar->beat;
        for (int i = 0; i < n; i += Q_CTRL) arp_tick(q, beat + bpf * (double)i);
        ar->beat += bpf * (double)n;
    } else if (ar->on_count) {
        arp_silence(q);
    }

    float tl[Q_BLOCK], tr[Q_BLOCK], ll[Q_BLOCK], lr[Q_BLOCK];
    memset(tl, 0, sizeof(float) * (size_t)n);
    memset(tr, 0, sizeof(float) * (size_t)n);
    memset(ll, 0, sizeof(float) * (size_t)n);
    memset(lr, 0, sizeof(float) * (size_t)n);
    q_tex_render(q, tl, tr, n);
    q_tex_bus(q, tl, tr, n);
    q_lines_render(q, ll, lr, n);
    q_mono_delay(q, ll, lr, n);

    /* routing: 0 = FX+Rev, 1 = Rev only, 2 = Dry */
    int txr = q_clampi((int)(q->ps[P_TX_FX] + 0.5f), 0, 2), lnr = q_clampi((int)(q->ps[P_LN_FX] + 0.5f), 0, 2);
    float fl[Q_BLOCK], fr[Q_BLOCK], rl[Q_BLOCK], rr[Q_BLOCK], ol[Q_BLOCK], orr[Q_BLOCK];
    for (int i = 0; i < n; i++) {
        fl[i] = (txr == 0 ? tl[i] : 0.0f) + (lnr == 0 ? ll[i] : 0.0f);
        fr[i] = (txr == 0 ? tr[i] : 0.0f) + (lnr == 0 ? lr[i] : 0.0f);
    }
    q_fx1(q, fl, fr, n);
    for (int i = 0; i < n; i++) {
        rl[i] = fl[i] + (txr == 1 ? tl[i] : 0.0f) + (lnr == 1 ? ll[i] : 0.0f);
        rr[i] = fr[i] + (txr == 1 ? tr[i] : 0.0f) + (lnr == 1 ? lr[i] : 0.0f);
        ol[i] = rl[i] + (txr == 2 ? tl[i] : 0.0f) + (lnr == 2 ? ll[i] : 0.0f);
        orr[i] = rr[i] + (txr == 2 ? tr[i] : 0.0f) + (lnr == 2 ? lr[i] : 0.0f);
    }
    q_reverb(q, rl, rr, ol, orr, n);

    float vol = q->ps[P_VOL];
    vol = vol * vol * 1.6f;
    if (q->sm_vol < 0.0f) q->sm_vol = vol;
    float g = q->sm_vol, dg = (vol - g) / (float)n;
    q->sm_vol = vol;
    float pk = q->peak * 0.995f;
    const float dcr = 0.9989f; /* ~8 Hz high-pass: phase modulation and asymmetric drive leave DC */
    for (int i = 0; i < n; i++) {
        g += dg;
        float xl = ol[i], xr = orr[i];
        float yl = xl - q->dc_x[0] + dcr * q->dc_y[0];
        float yr = xr - q->dc_x[1] + dcr * q->dc_y[1];
        q->dc_x[0] = xl; q->dc_y[0] = yl; q->dc_x[1] = xr; q->dc_y[1] = yr;
        float a = yl * g, b = yr * g;
        if (!(a == a)) a = 0.0f;
        if (!(b == b)) b = 0.0f;
        a = q_tanh(a);
        b = q_tanh(b);
        float m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
        if (m > pk) pk = m;
        out[2 * i] = (int16_t)(a * 32767.0f);
        out[2 * i + 1] = (int16_t)(b * 32767.0f);
    }
    q->peak = pk;
}

static void q_render(void *inst, int16_t *out, int frames) {
    QInstance *q = (QInstance *)inst;
    if (!q || !out || frames <= 0) return;
#ifdef QUARK_PROFILE
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
#endif
    if (q->host && q->host->get_bpm) {
        float b = q->host->get_bpm();
        if (b >= 20.0f && b <= 400.0f) q->bpm = b;
    }
    q->transport = 0;
    q->beat = -1.0;
    if (q->host && q->host->get_clock_status && q->host->get_clock_status() == MOVE_CLOCK_STATUS_RUNNING &&
        q->host->get_beat_position) {
        double b = q->host->get_beat_position();
        if (b >= 0.0) { q->beat = b; q->transport = 1; }
    }
    double bpf = (double)q->bpm / (60.0 * (double)q->sr);
    while (frames > 0) {
        int n = frames > Q_BLOCK ? Q_BLOCK : frames;
        render_chunk(q, out, n);
        out += 2 * n;
        frames -= n;
        if (q->transport) q->beat += bpf * (double)n;
    }
#ifdef QUARK_PROFILE
    clock_gettime(CLOCK_MONOTONIC, &t1);
    q->cpu_us += 0.05f * ((float)(t1.tv_sec - t0.tv_sec) * 1e6f + (float)(t1.tv_nsec - t0.tv_nsec) * 1e-3f - q->cpu_us);
#endif
}

static plugin_api_v2_t g_api = {
    MOVE_PLUGIN_API_VERSION_2, q_create, q_destroy, q_midi, q_set_param, q_get_param, q_get_error, q_render,
};

__attribute__((visibility("default"))) plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
