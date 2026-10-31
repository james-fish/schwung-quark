/* Native test + benchmark for Quark.
 *
 *   build/quark_test            run checks on every preset, print CPU numbers
 *   build/quark_test --wav DIR  also write one WAV per preset (8-note chord + a Lines line)
 *
 * Checks: no NaN/Inf, every preset makes sound, peaks stay below clipping, state round-trips, the
 * contract strings are valid-looking JSON. CPU: render time of 8 texture notes + 1 mono Lines voice
 * with all effects on, relative to real time on THIS machine (see the README for the Move estimate).
 */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under strict -std=c11 (glibc) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "plugin_api.h"
#include "params_gen.h"
#include "tables_gen.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static float host_bpm(void) { return 120.0f; }

static void midi(plugin_api_v2_t *api, void *in, int s, int d1, int d2) {
    uint8_t m[3] = {(uint8_t)s, (uint8_t)d1, (uint8_t)d2};
    api->on_midi(in, m, 3, 0);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void write_wav(const char *path, const int16_t *pcm, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int sr = 44100, bytes = frames * 4;
    uint32_t u;
    uint16_t s;
    fwrite("RIFF", 1, 4, f); u = 36 + (uint32_t)bytes; fwrite(&u, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); u = 16; fwrite(&u, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    u = (uint32_t)sr; fwrite(&u, 4, 1, f); u = (uint32_t)sr * 4; fwrite(&u, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); u = (uint32_t)bytes; fwrite(&u, 4, 1, f);
    fwrite(pcm, 4, (size_t)frames, f);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *wavdir = 0;
    if (argc > 2 && strcmp(argv[1], "--wav") == 0) wavdir = argv[2];
    host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    host.get_bpm = host_bpm;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    double t0 = now_s();
    void *in = api->create_instance("", 0);
    printf("create (incl. wavetable build): %.1f ms\n", (now_s() - t0) * 1000.0);
    CHECK(in != 0, "create_instance");
    if (!in) return 1;

    static char buf[65536];
    int n = api->get_param(in, "chain_params", buf, sizeof buf);
    CHECK(n > 1000 && buf[0] == '[' && buf[n - 1] == ']', "chain_params");
    n = api->get_param(in, "ui_hierarchy", buf, sizeof buf);
    CHECK(n > 100 && buf[0] == '{' && buf[n - 1] == '}', "ui_hierarchy");

    const int SECS = 6, FR = 44100 * SECS;
    static int16_t pcm[44100 * 6 * 2];
    int chord[8] = {48, 55, 60, 64, 67, 71, 74, 79};
    int line[8] = {36, 43, 48, 46, 41, 43, 39, 36};
    double worst = 0.0, total = 0.0;

    for (int p = 0; p < quark_preset_count; p++) {
        char v[16];
        snprintf(v, sizeof v, "%d", p);
        api->set_param(in, "all_notes_off", "1");
        int16_t tmp[256];
        for (int i = 0; i < 400; i++) api->render_block(in, tmp, 128);  /* let tails die */
        api->set_param(in, "preset", v);
        /* the benchmark scenario: 8 texture notes + a mono Lines line on top, everything audible */
        int cpu = wavdir == 0;
        if (cpu) {
            api->set_param(in, "ln_vol", "0.6");
            api->set_param(in, "key_mode", "0");
            api->set_param(in, "ln_mode", "1");
        }
        for (int i = 0; i < 8; i++) midi(api, in, 0x90, chord[i], 100);
        double r0 = now_s();
        double peak = 0.0, sum = 0.0;
        int bad = 0;
        for (int f = 0; f < FR; f += 128) {
            /* the Lines line: a new bass note every half second (legato mono) */
            if (cpu && f % (44100 / 2) == 0) midi(api, in, 0x90, line[(f / (44100 / 2)) % 8], 100);
            if (f == 44100 * 4) for (int i = 0; i < 8; i++) midi(api, in, 0x80, chord[i], 0);
            api->render_block(in, pcm + 2 * f, 128);
            for (int i = 0; i < 256; i++) {
                double x = pcm[2 * f + i] / 32768.0;
                if (x != x) bad++;
                if (fabs(x) > peak) peak = fabs(x);
                sum += x * x;
            }
        }
        double el = now_s() - r0;
        double rt = el / (double)SECS;
        total += rt;
        if (rt > worst) worst = rt;
        double rms = sqrt(sum / (FR * 2.0));
        printf("%2d %-16s peak %.3f rms %.3f  cpu %.2f%% of realtime\n", p, quark_preset_names[p], peak, rms, rt * 100.0);
        CHECK(bad == 0, "%s: NaN", quark_preset_names[p]);
        CHECK(peak > 0.02, "%s: silent", quark_preset_names[p]);
        CHECK(peak < 0.995, "%s: clipping (%.3f)", quark_preset_names[p], peak);
        if (wavdir) {
            char path[512];
            snprintf(path, sizeof path, "%s/%02d_%s.wav", wavdir, p, quark_preset_names[p]);
            for (char *c = path + strlen(wavdir) + 1; *c; c++) if (*c == ' ' || *c == '/') *c = '_';
            write_wav(path, pcm, FR);
        }
    }

    /* state round trip */
    api->set_param(in, "preset", "7");
    int sl = api->get_param(in, "state", buf, sizeof buf);
    CHECK(sl > 100, "state get");
    static char buf2[65536];
    api->set_param(in, "preset", "0");
    api->set_param(in, "state", buf);
    api->get_param(in, "state", buf2, sizeof buf2);
    CHECK(strcmp(buf, buf2) == 0, "state round trip");

    /* arp smoke test: runs, makes sound, stops when keys are released */
    api->set_param(in, "all_notes_off", "1");
    api->set_param(in, "preset", "21");
    for (int i = 0; i < 3; i++) midi(api, in, 0x90, 60 + 4 * i, 100);
    double apk = 0.0;
    for (int f = 0; f < 44100 * 2; f += 128) {
        api->render_block(in, pcm, 128);
        for (int i = 0; i < 256; i++) apk = fmax(apk, fabs(pcm[i] / 32768.0));
    }
    CHECK(apk > 0.02, "arp silent");
    for (int i = 0; i < 3; i++) midi(api, in, 0x80, 60 + 4 * i, 0);
    api->get_param(in, "voices", buf, sizeof buf);
    printf("voices after arp release: %s\n", buf);

    /* every Pads type and every Mono shape sounds, stays finite and below clipping */
    static int16_t blk[256];
    double sw_worst = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        int count = pass ? Q_NUM_LINE_SHAPES : Q_NUM_TEX_TYPES;
        for (int t = 0; t < count; t++) {
            char v[16];
            api->set_param(in, "all_notes_off", "1");
            api->set_param(in, "preset", "0");
            api->set_param(in, "arp_mode", "0");
            snprintf(v, sizeof v, "%d", t);
            if (pass) {
                api->set_param(in, "key_mode", "3");
                api->set_param(in, "ln_vol", "0.8");
                api->set_param(in, "ln_shape", v);
                api->set_param(in, "ln_var", "0.5");
                api->set_param(in, "ln_wenv", "0.4");
            } else {
                api->set_param(in, "tx_type", v);
                api->set_param(in, "tx_drive", "0.5");
            }
            for (int b = 0; b < 5; b++) {   /* the blocks after a switch build an Amino table */
                double s0 = now_s();
                api->render_block(in, blk, 128);
                double sw = now_s() - s0;
                if (sw > sw_worst) sw_worst = sw;
            }
            for (int i = 0; i < 4; i++) midi(api, in, 0x90, pass ? 36 + 12 * i : 48 + 5 * i, 110);
            double pk = 0.0;
            int bad = 0;
            for (int f = 0; f < 44100; f += 128) {
                if (f == 30000) midi(api, in, 0x90, 96, 127);   /* a high note: the top mips */
                api->render_block(in, blk, 128);
                for (int i = 0; i < 256; i++) { double x = blk[i] / 32768.0; if (x != x) bad++; pk = fmax(pk, fabs(x)); }
            }
            const char *what = pass ? "shape" : "type";
            CHECK(bad == 0, "%s %d NaN", what, t);
            CHECK(pk > 0.02, "%s %d silent (%.4f)", what, t, pk);
            CHECK(pk < 0.995, "%s %d clipping (%.3f)", what, t, pk);
            for (int i = 0; i < 4; i++) midi(api, in, 0x80, pass ? 36 + 12 * i : 48 + 5 * i, 0);
            midi(api, in, 0x80, 96, 0);
        }
    }
    printf("worst block right after a table switch: %.2f ms (block = 2.90 ms)\n", sw_worst * 1000.0);

    /* drive: all three positions at full drive with 12 voices */
    for (int dp = 0; dp < 3; dp++) {
        char v[8];
        api->set_param(in, "all_notes_off", "1");
        api->set_param(in, "preset", "8");
        api->set_param(in, "tx_voices", "12");
        api->set_param(in, "tx_drive", "1");
        snprintf(v, sizeof v, "%d", dp);
        api->set_param(in, "tx_drvpos", v);
        for (int i = 0; i < 12; i++) midi(api, in, 0x90, 40 + 3 * i, 127);
        double pk = 0.0, ss = 0.0;
        int bad = 0;
        for (int f = 0; f < 44100; f += 128) {
            api->render_block(in, blk, 128);
            for (int i = 0; i < 256; i++) { double x = blk[i] / 32768.0; if (x != x) bad++; pk = fmax(pk, fabs(x)); ss += x * x; }
        }
        printf("drive pos %d: peak %.3f rms %.3f\n", dp, pk, sqrt(ss / (44100 * 2.0)));
        CHECK(bad == 0 && pk > 0.05, "drive pos %d", dp);
        for (int i = 0; i < 12; i++) midi(api, in, 0x80, 40 + 3 * i, 0);
    }

    /* pads latch + the arp following the pad chord */
    midi(api, in, 0xB0, 120, 0);
    api->set_param(in, "preset", "26");   /* Chord Pluck Arp: keys -> Pads only, arp source = Pads chord */
    api->set_param(in, "tx_latch", "1");
    int c1[3] = {60, 64, 67}, c2[3] = {62, 65, 69};
    for (int i = 0; i < 3; i++) midi(api, in, 0x90, c1[i], 100);
    for (int i = 0; i < 3; i++) midi(api, in, 0x80, c1[i], 0);
    for (int f = 0; f < 44100; f += 128) api->render_block(in, blk, 128);
    api->get_param(in, "arp_notes", buf, sizeof buf);
    CHECK(strcmp(buf, "60,64,67") == 0, "latched chord feeds the arp (got %s)", buf);
    api->get_param(in, "voices", buf, sizeof buf);
    CHECK(buf[0] == '3' && strcmp(buf + 2, "0") != 0, "latched pads + arp voices (got %s)", buf);
    for (int i = 0; i < 3; i++) midi(api, in, 0x90, c2[i], 100);
    api->get_param(in, "arp_notes", buf, sizeof buf);
    CHECK(strcmp(buf, "62,65,69") == 0, "a new chord replaces the latched one (got %s)", buf);
    for (int i = 0; i < 3; i++) midi(api, in, 0x80, c2[i], 0);
    api->set_param(in, "tx_latch", "0");
    api->get_param(in, "arp_notes", buf, sizeof buf);
    CHECK(buf[0] == 0, "latch off releases the chord (got %s)", buf);
    api->set_param(in, "arp_hold", "1");
    for (int i = 0; i < 3; i++) midi(api, in, 0x90, c1[i], 100);
    for (int i = 0; i < 3; i++) {
        midi(api, in, 0x80, c1[i], 0);
        api->render_block(in, blk, 128);
    }
    api->get_param(in, "arp_notes", buf, sizeof buf);
    CHECK(strcmp(buf, "60,64,67") == 0, "arp latch keeps the last chord (got %s)", buf);

    /* arp types: the first notes each one plays from C E G (gate short so steps never overlap) */
    {
        const char *names[] = {"", "Up", "Down", "Up/Down", "Down/Up", "Random", "Order", "Chord", "Leap In", "Leap Out", "Pattern"};
        const char *expect[] = {"", "60 64 67 72 76 79 60", "79 76 72 67 64 60 79", "60 64 67 72 76 79 76", "79 76 72 67 64 60 64",
                                0, "64 60 67 76 72 79 64", "60+64+67 72+76+79 60+64+67", "60 79 64 76 67 72 60",
                                "67 72 64 76 60 79 67", "60+67 64 60+67"};
        for (int mode = 1; mode <= 10; mode++) {
            char v[8], seq[256] = "", last[64] = "";
            midi(api, in, 0xB0, 120, 0);
            api->set_param(in, "preset", "26");
            api->set_param(in, "arp_drop", "0");
            api->set_param(in, "arp_vrnd", "0");
            api->set_param(in, "arp_rhy", "0");
            api->set_param(in, "arp_gate", "0.3");
            api->set_param(in, "arp_oct", mode == 10 ? "0" : "1");
            api->set_param(in, "arp_pat", "16");    /* (1-3)-(2-4): with three notes, 4 folds to 2 */
            snprintf(v, sizeof v, "%d", mode);
            api->set_param(in, "arp_mode", v);
            int order[3] = {64, 60, 67};
            for (int i = 0; i < 3; i++) midi(api, in, 0x90, order[i], 100);
            int got = 0;
            for (int f = 0; f < 44100 * 6 && got < 7; f += 128) {
                api->render_block(in, blk, 128);
                api->get_param(in, "arp_playing", buf, sizeof buf);
                if (buf[0] && strcmp(buf, last) != 0) {
                    for (char *c = buf; *c; c++) if (*c == ',') *c = '+';
                    /* sort chord members for a stable comparison: they are emitted low to high already */
                    if (got) strcat(seq, " ");
                    strcat(seq, buf);
                    got++;
                }
                api->get_param(in, "arp_playing", last, sizeof last);
            }
            if (mode == 7 || mode == 10) { char *p3 = seq; int k = 0; while (*p3 && k < 3) { if (*p3 == ' ') k++; if (k == 3) { *p3 = 0; break; } p3++; } }
            printf("arp %-9s %s\n", names[mode], seq);
            if (expect[mode]) CHECK(strncmp(seq, expect[mode], strlen(expect[mode])) == 0, "arp %s: got %s, want %s", names[mode], seq, expect[mode]);
            for (int i = 0; i < 3; i++) midi(api, in, 0x80, order[i], 0);
        }
        /* Drop 100%: nothing plays; Drop 0: plays */
        midi(api, in, 0xB0, 120, 0);
        api->set_param(in, "preset", "26");
        api->set_param(in, "arp_drop", "1");
        for (int i = 0; i < 3; i++) midi(api, in, 0x90, 60 + 4 * i, 100);
        int any = 0;
        for (int f = 0; f < 44100; f += 128) { api->render_block(in, blk, 128); api->get_param(in, "arp_playing", buf, sizeof buf); any |= buf[0] != 0; }
        CHECK(!any, "arp drop 100%% still played");
        for (int i = 0; i < 3; i++) midi(api, in, 0x80, 60 + 4 * i, 0);
    }

    /* ---- chord stacks: the table plays at the note (its cycle is DIV periods), no sub below the root
     * until the "Sub" frame, nothing a semitone off. Goertzel bins over 1 s of A3. ---- */
    {
        static int16_t cb[256];
        midi(api, in, 0xB0, 120, 0);
        api->set_param(in, "preset", "0");
        const char *cset[][2] = {{"key_mode", "2"}, {"rv_amt", "0"}, {"fx_amt", "0"}, {"sh_amt", "0"}, {"ens_amt", "0"},
                                 {"tx_a", "0"}, {"tx_s", "1"}, {"flt_type", "0"}, {"vintage", "0"}, {"tx_anim", "0"},
                                 {"tx_spread", "0"}, {"tx_detune", "0"}, {"vib", "0"}, {"tx_depth", "0"}, {"tx_drive", "0"}};
        for (size_t k = 0; k < sizeof cset / sizeof cset[0]; k++) api->set_param(in, cset[k][0], cset[k][1]);
        const double fq[4] = {110.0, 220.0, 220.0 * 1.0594631, 330.0};
        for (int c = 0; c < Q_CH_COUNT; c++) {
            int ty = -1;
            for (int t = 0; t < Q_NUM_TEX_TYPES; t++) if (Q_TEX_FAMILY[t] == 1 && Q_TEX_INDEX[t] == Q_WT_COUNT + c) ty = t;
            CHECK(ty >= 0, "chord table %d has a Pads type", c);
            for (int fr = 0; fr < 2; fr++) {
                char tv[8]; snprintf(tv, sizeof tv, "%d", ty); api->set_param(in, "tx_type", tv);
                api->set_param(in, "tx_var", fr ? "1" : "0");
                for (int i = 0; i < 40; i++) api->render_block(in, cb, 128);
                midi(api, in, 0x90, 57, 110);
                for (int i = 0; i < 100; i++) api->render_block(in, cb, 128);
                double re[4] = {0}, im[4] = {0}; long nn = 0; int bad = 0;
                for (int f = 0; f < 44100; f += 128) {
                    api->render_block(in, cb, 128);
                    for (int i = 0; i < 128; i++, nn++) {
                        double x = cb[2 * i] / 32768.0;
                        if (x != x) bad++;
                        for (int b = 0; b < 4; b++) { double w = 2 * M_PI * fq[b] * nn / 44100.0; re[b] += x * cos(w); im[b] += x * sin(w); }
                    }
                }
                midi(api, in, 0x80, 57, 0);
                for (int i = 0; i < 200; i++) api->render_block(in, cb, 128);
                double g[4];
                for (int b = 0; b < 4; b++) g[b] = sqrt(re[b] * re[b] + im[b] * im[b]) / nn;
                printf("chord %d frame %d: 110 %.4f  220 %.4f  233 %.4f  330 %.4f\n", c, fr ? 7 : 0, g[0], g[1], g[2], g[3]);
                CHECK(!bad && g[1] > 0.004, "chord %d sounds at the note", c);
                CHECK(g[1] > 8 * g[2], "chord %d: nothing a semitone up", c);
                if (fr == 0) CHECK(g[1] > 8 * g[0], "chord %d frame 0: no sub-octave", c);
                else CHECK(g[0] > 0.1 * g[1], "chord %d last frame: the sub octave is there", c);
            }
        }
    }

    /* ---- every Pads filter type: a chord under a full cutoff sweep, at three resonances ---- */
    {
        static int16_t fb2[256];
        midi(api, in, 0xB0, 120, 0);
        api->set_param(in, "preset", "0");
        const char *fset[][2] = {{"key_mode", "2"}, {"tx_type", "0"}, {"rv_amt", "0"}, {"fx_amt", "0"}, {"sh_amt", "0"},
                                 {"ens_amt", "0"}, {"tx_a", "0"}, {"tx_s", "1"}, {"flt_env", "0"}, {"vintage", "1"}};
        for (size_t k = 0; k < sizeof fset / sizeof fset[0]; k++) api->set_param(in, fset[k][0], fset[k][1]);
        for (int ft = 0; ft < Q_NUM_FILTER_TYPES; ft++) {
            double tpk = 0.0, trms = 0.0; int bad = 0;
            for (int ri = 0; ri < 3; ri++) {
                char rv[8], fv[8];
                snprintf(fv, sizeof fv, "%d", ft); api->set_param(in, "flt_type", fv);
                snprintf(rv, sizeof rv, "%.1f", ri * 0.5); api->set_param(in, "flt_res", rv);
                for (int i = 0; i < 6; i++) midi(api, in, 0x90, 40 + 5 * i, 110);
                double ss = 0.0; int cnt = 0;
                for (int f = 0; f < 44100; f += 128) {
                    char cv[16]; snprintf(cv, sizeof cv, "%.3f", (double)f / 44100.0);
                    api->set_param(in, "flt_cut", cv);
                    api->render_block(in, fb2, 128);
                    for (int i = 0; i < 256; i++) { double x = fb2[i] / 32768.0; if (x != x) bad++; tpk = fmax(tpk, fabs(x)); ss += x * x; cnt++; }
                }
                trms = fmax(trms, sqrt(ss / cnt));
                for (int i = 0; i < 6; i++) midi(api, in, 0x80, 40 + 5 * i, 0);
                midi(api, in, 0xB0, 120, 0);
                for (int i = 0; i < 50; i++) api->render_block(in, fb2, 128);
            }
            printf("filter %-2d peak %.3f rms %.3f\n", ft, tpk, trms);
            CHECK(bad == 0 && trms > 0.005 && trms < 0.45, "filter type %d sane (peaks ride the output tanh at full resonance)", ft);
        }
        api->set_param(in, "vintage", "0.25");
    }

    /* ---- Mono: Wave Env stays in the table, Spread, Delay ---- */
    {
        static int16_t mb[44100 * 2 * 2];
        const char *setup[][2] = {{"preset", "0"}, {"key_mode", "3"}, {"ln_mode", "0"}, {"ln_vol", "0.8"},
                                  {"ln_fx", "2"}, {"rv_amt", "0"}, {"fx_amt", "0"}, {"sh_amt", "0"}, {"ln_sus", "0"},
                                  {"ln_a", "0"}, {"ln_d", "0.15"}, {"ln_cut", "1"}, {"ln_fenv", "0"}};
        midi(api, in, 0xB0, 120, 0);
        for (int i = 0; i < 400; i++) api->render_block(in, mb, 128);
        for (size_t k = 0; k < sizeof setup / sizeof setup[0]; k++) api->set_param(in, setup[k][0], setup[k][1]);
        /* every Amino table at Variation 70% + Wave Env 80%: the sweep must stay inside the table */
        int worst_t = -1; double worst_r = 0.0;
        for (int t = 0; t < Q_NUM_WT_AMINO; t++) {
            snprintf(buf, sizeof buf, "%d", Q_LINE_VA + Q_NUM_WT_QUARK + t);
            api->set_param(in, "ln_shape", buf);
            api->set_param(in, "ln_var", "0.7");
            api->set_param(in, "ln_wenv", "0.8");
            for (int i = 0; i < 8; i++) api->render_block(in, mb, 128);   /* let the table build */
            midi(api, in, 0x90, 48, 110);
            double pk = 0.0, d2 = 0.0; int bad = 0;
            for (int f = 0; f < 22050; f += 128) {
                api->render_block(in, mb, 128);
                for (int i = 2; i < 128; i++) {
                    double x = mb[2 * i] / 32768.0, x1 = mb[2 * i - 2] / 32768.0, x2 = mb[2 * i - 4] / 32768.0;
                    if (x != x) bad++;
                    pk = fmax(pk, fabs(x));
                    d2 = fmax(d2, fabs(x - 2 * x1 + x2));
                }
            }
            midi(api, in, 0x80, 48, 0);
            for (int i = 0; i < 100; i++) api->render_block(in, mb, 128);
            CHECK(bad == 0 && pk > 0.01, "mono wave env table %d sounds", t);
            if (pk > 0.0 && d2 / pk > worst_r) { worst_r = d2 / pk; worst_t = t; }
        }
        printf("mono wave env 70%%+80%%: worst 2nd-difference / peak %.3f (table %d)\n", worst_r, worst_t);
        api->set_param(in, "ln_shape", "0");
        api->set_param(in, "ln_wenv", "0");
        api->set_param(in, "ln_var", "0");   /* no stereo tremolo while measuring balance */

        /* Spread: centre = equal channels; steps = notes land on different sides */
        double bal[2][4];
        for (int sp = 0; sp < 2; sp++) {
            api->set_param(in, "ln_spread", sp ? "1" : "0");
            for (int i = 0; i < 100; i++) api->render_block(in, mb, 128);   /* let the smoothed params settle */
            for (int nn = 0; nn < 4; nn++) {
                double el = 0.0, er = 0.0;
                midi(api, in, 0x90, 48 + nn, 110);
                for (int f = 0; f < 8820; f += 128) {
                    api->render_block(in, mb, 128);
                    for (int i = 0; i < 128; i++) { el += (double)mb[2 * i] * mb[2 * i]; er += (double)mb[2 * i + 1] * mb[2 * i + 1]; }
                }
                midi(api, in, 0x80, 48 + nn, 0);
                for (int i = 0; i < 100; i++) api->render_block(in, mb, 128);
                bal[sp][nn] = (er - el) / (er + el + 1e-9);
            }
        }
        printf("mono spread balance  centre %.2f %.2f %.2f %.2f   steps %.2f %.2f %.2f %.2f\n", bal[0][0], bal[0][1],
               bal[0][2], bal[0][3], bal[1][0], bal[1][1], bal[1][2], bal[1][3]);
        CHECK(fabs(bal[0][0]) < 0.02 && fabs(bal[0][3]) < 0.02, "spread 0 is centred");
        {
            double lo = 1.0, hi = -1.0;
            for (int nn = 0; nn < 4; nn++) { lo = fmin(lo, bal[1][nn]); hi = fmax(hi, bal[1][nn]); }
            CHECK(lo < -0.3 && hi > 0.3, "spread steps notes across the field");
        }
        api->set_param(in, "ln_spread", "0");

        /* Delay: a short blip, then energy in the window where the 1/8 echo (250 ms at 120) lands */
        double echo[2];
        for (int dd = 0; dd < 2; dd++) {
            api->set_param(in, "ln_dly", dd ? "0.5" : "0");
            api->set_param(in, "ln_dtime", "5");
            for (int i = 0; i < 300; i++) api->render_block(in, mb, 128);
            midi(api, in, 0x90, 60, 120);
            int pos = 0; double e = 0.0; int bad = 0;
            for (int f = 0; f < 44100; f += 128, pos += 128) {
                if (pos == 128 * 10) midi(api, in, 0x80, 60, 0);
                api->render_block(in, mb, 128);
                for (int i = 0; i < 256; i++) {
                    double x = mb[i] / 32768.0;
                    if (x != x) bad++;
                    if (pos >= 10800 && pos < 13230) e += x * x;   /* 245-300 ms: the first echo */
                }
            }
            echo[dd] = e;
            CHECK(bad == 0, "mono delay finite");
        }
        printf("mono delay: echo window energy off %.5f on %.5f\n", echo[0], echo[1]);
        CHECK(echo[1] > 20.0 * echo[0] + 1e-4, "mono delay repeats");
        api->set_param(in, "ln_dly", "0");
    }

    printf("\nCPU (this machine): mean %.2f%%, worst %.2f%% of one core for 8 texture voices + mono Lines + FX\n",
           total / quark_preset_count * 100.0, worst * 100.0);
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    api->destroy_instance(in);
    return g_fail ? 1 : 0;
}
