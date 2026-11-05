/* level PRESET [key=val ...]: a fresh instance plays the 8-note chord for 4 s; prints the loudest 0.4 s
 * (100 Hz high-passed RMS) and the peak, both in dB. Used by tools/level_presets.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_api.h"
#include "params_gen.h"
extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);
static float host_bpm(void) { return 120.0f; }
int main(int argc, char **argv) {
    host_api_v1_t host; memset(&host, 0, sizeof host);
    host.api_version = MOVE_PLUGIN_API_VERSION; host.sample_rate = 44100; host.frames_per_block = 128;
    host.get_bpm = host_bpm;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *in = api->create_instance("", 0);
    int k = 0;
    for (; k < quark_preset_count; k++) if (!strcmp(quark_preset_names[k], argv[1])) break;
    if (k == quark_preset_count) { fprintf(stderr, "no preset %s\n", argv[1]); return 1; }
    char v[16]; snprintf(v, sizeof v, "%d", k); api->set_param(in, "preset", v);
    for (int i = 2; i < argc; i++) { char *e = strchr(argv[i], '='); *e = 0; api->set_param(in, argv[i], e + 1); }
    int chord[8] = {48, 55, 60, 64, 67, 71, 74, 79};
    for (int i = 0; i < 8; i++) { uint8_t m[3] = {0x90, (uint8_t)chord[i], 100}; api->on_midi(in, m, 3, 0); }
    const int N = 44100 * 4; static int16_t pcm[44100 * 4 * 2]; static double y[44100 * 4];
    for (int f = 0; f < N; f += 128) api->render_block(in, pcm + 2 * f, 128);
    double a = exp(-2 * M_PI * 100 / 44100.0), py = 0, px = 0, pk = 0;
    for (int i = 0; i < N; i++) {
        double x = (pcm[2 * i] + pcm[2 * i + 1]) / 65536.0;
        if (fabs(pcm[2 * i] / 32768.0) > pk) pk = fabs(pcm[2 * i] / 32768.0);
        if (fabs(pcm[2 * i + 1] / 32768.0) > pk) pk = fabs(pcm[2 * i + 1] / 32768.0);
        py = a * (py + x - px); px = x; y[i] = py;
    }
    int w = (int)(0.4 * 44100); double s = 0, best = 0;
    for (int i = 0; i < N; i++) { s += y[i] * y[i]; if (i >= w) s -= y[i - w] * y[i - w]; if (i >= w - 1 && s > best) best = s; }
    printf("%.1f %.1f\n", 10 * log10(best / w + 1e-18), 20 * log10(pk + 1e-9));
    return 0;
}
