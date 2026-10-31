/* WebAssembly shell around the Quark engine (browser test page only; not shipped to the Move).
 * Adapted from Amino/Crest's web/wasm_host.c (MIT). The same src/dsp sources that build dsp.so are
 * compiled to wasm32; this file is a one-instance Schwung stand-in with fixed scratch buffers so
 * JavaScript needs no malloc. web/worklet.js drives it from an AudioWorklet. */
#include <stdint.h>
#include <string.h>

#include "plugin_api.h"

extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

#define EXPORT __attribute__((used, visibility("default")))
#define KEY_CAP 256
#define VAL_CAP 65536
#define OUT_CAP 65536

static host_api_v1_t g_host;
static plugin_api_v2_t *g_api;
static void *g_inst;
static float g_bpm = 120.0f;
static int g_running;
static double g_beats;
static int g_sr = 44100;

static char g_key[KEY_CAP];
static char g_val[VAL_CAP];
static char g_out[OUT_CAP];
static int16_t g_pcm[128 * 2];

static float host_bpm(void) { return g_bpm; }
static int host_clock(void) { return g_running ? MOVE_CLOCK_STATUS_RUNNING : MOVE_CLOCK_STATUS_STOPPED; }
static double host_beats(void) { return g_running ? g_beats : -1.0; }

EXPORT char *wa_key(void) { return g_key; }
EXPORT char *wa_val(void) { return g_val; }
EXPORT char *wa_out(void) { return g_out; }
EXPORT int16_t *wa_pcm(void) { return g_pcm; }
EXPORT int wa_key_cap(void) { return KEY_CAP; }
EXPORT int wa_val_cap(void) { return VAL_CAP; }

EXPORT int wa_init(int sample_rate) {
    if (g_inst) return 1;
    g_sr = sample_rate > 0 ? sample_rate : 44100;
    memset(&g_host, 0, sizeof g_host);
    g_host.api_version = MOVE_PLUGIN_API_VERSION;
    g_host.sample_rate = g_sr;
    g_host.frames_per_block = 128;
    g_host.get_bpm = host_bpm;
    g_host.get_clock_status = host_clock;
    g_host.get_beat_position = host_beats;
    g_api = move_plugin_init_v2(&g_host);
    if (!g_api) return 0;
    g_inst = g_api->create_instance("", 0);
    return g_inst ? 1 : 0;
}

EXPORT void wa_set_tempo(float bpm, int running) {
    g_bpm = bpm > 20.0f ? bpm : 120.0f;
    if (running && !g_running) g_beats = 0.0;
    g_running = running ? 1 : 0;
}

EXPORT void wa_set(void) {
    g_key[KEY_CAP - 1] = 0;
    g_val[VAL_CAP - 1] = 0;
    if (g_inst) g_api->set_param(g_inst, g_key, g_val);
}

EXPORT int wa_get(void) {
    g_key[KEY_CAP - 1] = 0;
    if (!g_inst) return -1;
    g_out[0] = 0;
    return g_api->get_param(g_inst, g_key, g_out, OUT_CAP);
}

EXPORT void wa_midi(int status, int d1, int d2) {
    if (!g_inst) return;
    uint8_t msg[3] = {(uint8_t)status, (uint8_t)d1, (uint8_t)d2};
    int len = (status & 0xF0) == 0xD0 || (status & 0xF0) == 0xC0 ? 2 : 3;
    g_api->on_midi(g_inst, msg, len, 0);
}

EXPORT void wa_render(int frames) {
    if (!g_inst) return;
    if (frames > 128) frames = 128;
    g_api->render_block(g_inst, g_pcm, frames);
    if (g_running) g_beats += (double)frames * (double)g_bpm / (60.0 * g_sr);
}
