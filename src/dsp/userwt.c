/* User wavetables: WAV files from the user folder become extra Pads Types / Mono Shapes.
 *
 * Format: the common single-cycle wavetable WAV, as Serum and Vital write it. Frames of 2048 samples
 * by default, or the frame size from a Serum "clm " chunk; a file shorter than 4096 samples that is
 * not a whole number of 2048-sample frames is one single cycle of any length. PCM 8/16/24/32-bit or
 * 32-bit float; the first channel is used. Up to Q_UW_FRAMES frames are kept (longer tables are
 * sampled evenly), each as its first Q_UW_H harmonics.
 *
 * Threads: q_uw_open (at create) lists the folder, sorted, and names the slots; that is all it does
 * before returning, so chain_params can name the files at once. A worker thread (SCHED_OTHER) then
 * reads the files and, whenever a layer asks for a table, band-limits it into that layer's back
 * buffer and swaps it in. The audio thread never allocates, locks or touches a file.
 */
#define _POSIX_C_SOURCE 200809L
#include "quark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Q_UW_DIR "/data/UserData/UserLibrary/Samples/Schwung/Wavetables"
#define Q_UW_FILE_MAX (16 << 20)

#ifndef QUARK_WASM
#include <dirent.h>
#include <pthread.h>
#include <time.h>

static const char *uw_dir(void) {
    const char *e = getenv("QUARK_WT_DIR");   /* tests */
    return e && *e ? e : Q_UW_DIR;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int is_wav(const char *n) {
    size_t l = strlen(n);
    if (l < 5 || n[0] == '.') return 0;
    const char *e = n + l - 4;
    return e[0] == '.' && lower(e[1]) == 'w' && lower(e[2]) == 'a' && lower(e[3]) == 'v';
}

static int name_cmp(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    for (;; x++, y++) {
        int d = lower((unsigned char)*x) - lower((unsigned char)*y);
        if (d || !*x) return d;
    }
}

/* display name: the file name without .wav, printable ASCII without quotes or backslashes */
static void make_names(const char *file, char *name, char *shortn) {
    size_t l = strlen(file) - 4, k = 0;
    for (size_t i = 0; i < l && k < Q_UW_NAME - 1; i++) {
        unsigned char c = (unsigned char)file[i];
        if (c == '_') c = ' ';
        if (c < 32 || c > 126 || c == '"' || c == '\\') continue;
        if (c == ' ' && (k == 0 || name[k - 1] == ' ')) continue;
        name[k++] = (char)c;
    }
    while (k > 0 && name[k - 1] == ' ') k--;
    if (!k) { memcpy(name, "User", 5); k = 4; }
    name[k] = 0;
    int j = 0;
    for (size_t i = 0; name[i] && j < 4; i++)
        if (name[i] != ' ') shortn[j++] = name[i];
    shortn[j] = 0;
}

/* ---- WAV ---- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* -> malloc'd mono float samples (first channel), *frame_len from a "clm " chunk or 0 */
static float *wav_read(const char *path, int *count, int *frame_len) {
    *count = 0;
    *frame_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > Q_UW_FILE_MAX) { fclose(f); return 0; }
    uint8_t *d = (uint8_t *)malloc((size_t)sz);
    if (!d) { fclose(f); return 0; }
    size_t got = fread(d, 1, (size_t)sz, f);
    fclose(f);
    float *out = 0;
    if (got != (size_t)sz || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) goto done;
    int fmt = 0, ch = 0, bits = 0;
    const uint8_t *data = 0;
    uint32_t data_len = 0;
    for (long p = 12; p + 8 <= sz;) {
        uint32_t len = rd32(d + p + 4);
        const uint8_t *c = d + p + 8;
        if ((long)len > sz - p - 8) len = (uint32_t)(sz - p - 8);
        if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
            fmt = rd16(c);
            ch = rd16(c + 2);
            bits = rd16(c + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(c + 24);   /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(d + p, "data", 4)) {
            data = c;
            data_len = len;
        } else if (!memcmp(d + p, "clm ", 4) && len > 3) {
            /* Serum: "<!>2048 ..." */
            char t[16] = {0};
            memcpy(t, c, len < 15 ? len : 15);
            if (t[0] == '<' && t[1] == '!' && t[2] == '>') *frame_len = atoi(t + 3);
        }
        p += 8 + (long)len + (len & 1);
    }
    if (!data || ch < 1 || (fmt != 1 && fmt != 3) || (fmt == 3 && bits != 32) ||
        (fmt == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32))
        goto done;
    int bps = bits / 8, stride = bps * ch;
    int n = (int)(data_len / (uint32_t)stride);
    if (n < 2) goto done;
    out = (float *)malloc(sizeof(float) * (size_t)n);
    if (!out) goto done;
    for (int i = 0; i < n; i++) {
        const uint8_t *s = data + (size_t)i * (size_t)stride;
        float v;
        if (fmt == 3) { uint32_t u = rd32(s); memcpy(&v, &u, 4); }
        else if (bits == 8) v = ((float)s[0] - 128.0f) / 128.0f;
        else if (bits == 16) v = (float)(int16_t)rd16(s) / 32768.0f;
        else if (bits == 24) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
        else v = (float)(int32_t)rd32(s) / 2147483648.0f;
        out[i] = v == v ? v : 0.0f;
    }
    *count = n;
done:
    free(d);
    return out;
}

/* read a file into t->spec; 1 on success */
static int uw_load(QUserTable *t, const char *path) {
    int n, flen;
    float *x = wav_read(path, &n, &flen);
    if (!x) return 0;
    int L;
    if (flen >= 16 && flen <= 8192 && n >= flen) L = flen;
    else if (n >= 2048 && n % 2048 == 0) L = 2048;
    else if (n <= 4096) L = n;            /* one single cycle */
    else L = 2048;
    int F = n / L;
    int nf = F < Q_UW_FRAMES ? F : Q_UW_FRAMES;
    float *spec = (float *)calloc((size_t)(nf < 2 ? 2 : nf) * 2 * Q_UW_H, sizeof(float));
    if (!spec) { free(x); return 0; }
    float pk = 0.0f;
    for (int k = 0; k < nf; k++) {
        int src = nf > 1 ? (int)((long)k * (F - 1) / (nf - 1)) : 0;
        const float *fr = x + (size_t)src * (size_t)L;
        q_uw_spectrum(fr, L, spec + (size_t)k * 2 * Q_UW_H);
        float mean = 0.0f;
        for (int i = 0; i < L; i++) mean += fr[i];
        mean /= (float)L;
        for (int i = 0; i < L; i++) pk = fmaxf(pk, fabsf(fr[i] - mean));
    }
    free(x);
    if (nf < 2) { memcpy(spec + 2 * Q_UW_H, spec, sizeof(float) * 2 * Q_UW_H); nf = 2; }
    /* the whole table to a 0.95 peak (DC is gone), so frames keep their relative levels */
    float sc = pk > 1e-5f ? 0.95f / pk : 0.0f;
    for (int i = 0; i < nf * 2 * Q_UW_H; i++) spec[i] *= sc;
    t->spec = spec;
    t->nf = nf;
    return 1;
}

typedef struct {
    QInstance *q;
    char paths[Q_UW_MAX][320];
    pthread_t th;
} UwWorker;

static void nap_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, 0);
}

static void *uw_main(void *arg) {
    UwWorker *wk = (UwWorker *)arg;
    QInstance *q = wk->q;
    for (int i = 0; i < q->uw_n && !q->uw_quit; i++)
        __atomic_store_n(&q->uw[i].ready, uw_load(&q->uw[i], wk->paths[i]) ? 1 : -1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&q->uw_quit, __ATOMIC_ACQUIRE)) {
        int idle = 1;
        for (int s = 0; s < 2; s++) {
            QUwSlot *u = &q->uws[s];
            int want = __atomic_load_n(&u->want, __ATOMIC_ACQUIRE);
            int f = __atomic_load_n(&u->front, __ATOMIC_ACQUIRE);
            if (want < 0 || want >= q->uw_n || u->have[f] == want) continue;
            if (__atomic_load_n(&q->uw[want].ready, __ATOMIC_ACQUIRE) != 1) continue;
            if (u->have[f] >= 0 && __atomic_load_n(&u->inuse, __ATOMIC_ACQUIRE) != f) continue; /* audio not on f yet */
            int b = f ^ 1;
            if (!u->buf[b]) u->buf[b] = (float *)malloc(sizeof(float) * Q_UW_FRAMES * Q_FRAME_FLOATS);
            if (!u->buf[b]) continue;
            QUserTable *t = &q->uw[want];
            u->have[b] = -1;
            for (int k = 0; k < t->nf; k++)
                q_uw_build_frame(t->spec + (size_t)k * 2 * Q_UW_H, u->buf[b] + (size_t)k * Q_FRAME_FLOATS);
            u->have[b] = want;
            __atomic_store_n(&u->front, b, __ATOMIC_RELEASE);
            idle = 0;
        }
        nap_ms(idle ? 10 : 1);
    }
    return 0;
}

/* chain_params with the user slots filled in: ,"<name>" in options, ,"<short>" in short_options */
static char *build_contract(const QInstance *q) {
    size_t len = 1;
    for (int i = 0; i < quark_cp_nfrag; i++) len += strlen(quark_cp_frag[i]);
    len += (size_t)4 * Q_UW_MAX * (Q_UW_NAME + 4);
    char *o = (char *)malloc(len), *p = o;
    if (!o) return 0;
    for (int i = 0; i < quark_cp_nfrag; i++) {
        size_t l = strlen(quark_cp_frag[i]);
        memcpy(p, quark_cp_frag[i], l);
        p += l;
        if (i == quark_cp_nfrag - 1) break;
        int h = quark_cp_hole[i], k = h % 100;
        if (k < q->uw_n) p += sprintf(p, ",\"%s\"", h >= 100 ? q->uw[k].shortn : q->uw[k].name);
    }
    *p = 0;
    return o;
}

void q_uw_open(QInstance *q) {
    for (int s = 0; s < 2; s++) { q->uws[s].have[0] = q->uws[s].have[1] = -1; q->uws[s].want = -1; }
    const char *dir = uw_dir();
    DIR *dh = opendir(dir);
    if (!dh) return;
    char *names[256];
    int nn = 0;
    struct dirent *de;
    while ((de = readdir(dh)) && nn < 256)
        if (is_wav(de->d_name) && strlen(de->d_name) < 255) names[nn++] = strdup(de->d_name);
    closedir(dh);
    qsort(names, (size_t)nn, sizeof(char *), name_cmp);
    UwWorker *wk = (UwWorker *)calloc(1, sizeof *wk);
    for (int i = 0; i < nn; i++) {
        if (wk && q->uw_n < Q_UW_MAX) {
            QUserTable *t = &q->uw[q->uw_n];
            make_names(names[i], t->name, t->shortn);
            snprintf(wk->paths[q->uw_n], sizeof wk->paths[0], "%s/%s", dir, names[i]);
            q->uw_n++;
        }
        free(names[i]);
    }
    if (!wk) return;
    if (!q->uw_n) { free(wk); return; }
    q->cp_dyn = build_contract(q);
    wk->q = q;
    if (pthread_create(&wk->th, 0, uw_main, wk) != 0) { free(wk); return; }
    q->uw_thread = wk;
}

void q_uw_close(QInstance *q) {
    UwWorker *wk = (UwWorker *)q->uw_thread;
    if (wk) {
        __atomic_store_n(&q->uw_quit, 1, __ATOMIC_RELEASE);
        pthread_join(wk->th, 0);
        free(wk);
        q->uw_thread = 0;
    }
    for (int i = 0; i < Q_UW_MAX; i++) { free(q->uw[i].spec); q->uw[i].spec = 0; }
    for (int s = 0; s < 2; s++)
        for (int b = 0; b < 2; b++) { free(q->uws[s].buf[b]); q->uws[s].buf[b] = 0; }
    free(q->cp_dyn);
    q->cp_dyn = 0;
    q->uw_n = 0;
}

#else /* the browser build has no user folder */

void q_uw_open(QInstance *q) {
    for (int s = 0; s < 2; s++) { q->uws[s].have[0] = q->uws[s].have[1] = -1; q->uws[s].want = -1; }
}
void q_uw_close(QInstance *q) { (void)q; }

#endif

int q_uw_find(const QInstance *q, const char *name) {
    for (int i = 0; i < q->uw_n; i++)
        if (strcmp(q->uw[i].name, name) == 0) return i;
    return -1;
}
