// Ibmxchord final-output FX: feedback delay + a small 4-comb Schroeder-ish
// reverb, applied to the stereo mix in the I2S feed task.
//
// Delay: one circular buffer per channel, feedback = amount/10. Wet/dry mix
//   so 0 stays fully dry.
// Reverb: 4 parallel combs (mono-ish, summed into both channels) with
//   feedback = amount/10, mixed into the dry signal.
//
// CPU cost is ~12 mul/adds per sample per effect — trivial at 44.1 kHz on
// the S3's 240 MHz core.

#include "fx.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

static int g_sample_rate = 44100;
// effective delay length in samples (allocated buffer is max-length)
static inline int sample_rate_delay_len(void) { return g_sample_rate * fx_delay_ms / 1000; }

int fx_delay_amt  = 0;
int fx_delay_ms   = 160;
int fx_reverb_amt = 0;
int fx_volume = 4;              // 0..9; step 4 = unity (x1.00)

#define REV_COMBS 4

// ---- delay line ----
static int16_t *dl_buf[2] = {nullptr, nullptr};
static int       dl_len = 0;
static int       dl_pos = 0;

// ---- reverb combs (one bank, shared by both channels via crossfeed) ----
static int16_t *rv_buf[REV_COMBS];
static int      rv_len[REV_COMBS];
static int      rv_pos[REV_COMBS];

static void alloc_line(int16_t **buf, int len) {
    free(*buf);
    *buf = (int16_t *)malloc(len * sizeof(int16_t));
    if (*buf) memset(*buf, 0, len * sizeof(int16_t));
}

void fx_init(int sample_rate) {
    g_sample_rate = sample_rate;
    // delay: fixed max allocation, effective length set by fx_delay_ms
    const int DL_MAX = sample_rate * 400 / 1000;
    alloc_line(&dl_buf[0], DL_MAX);
    alloc_line(&dl_buf[1], DL_MAX);
    dl_len = DL_MAX;
    // reverb combs at classic Schroeder spacings (ms)
    static const int comb_ms[REV_COMBS] = {29, 37, 43, 53};
    for (int i = 0; i < REV_COMBS; i++) {
        rv_len[i] = sample_rate * comb_ms[i] / 1000;
        alloc_line(&rv_buf[i], rv_len[i]);
        rv_pos[i] = 0;
    }
    dl_pos = 0;
}

void fx_reset() {
    if (dl_buf[0]) memset(dl_buf[0], 0, dl_len * sizeof(int16_t));
    if (dl_buf[1]) memset(dl_buf[1], 0, dl_len * sizeof(int16_t));
    for (int i = 0; i < REV_COMBS; i++)
        if (rv_buf[i]) memset(rv_buf[i], 0, rv_len[i] * sizeof(int16_t));
}

// Output gain in 1/256ths for steps 0..9 (4 = unity).
static const uint16_t VOL_GAIN[10] = { 64, 90, 128, 179, 256, 307, 358, 410, 461, 512 };

static inline int16_t sat(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void fx_process(int16_t *stereo, int frames) {
    if (!dl_buf[0]) return;
    const int dlen = sample_rate_delay_len();   // effective delay in samples
    const int dfb = fx_delay_amt;               // /10 feedback
    const int rvb = fx_reverb_amt;              // /10 feedback + wet

    for (int f = 0; f < frames; f++) {
        int16_t *L = &stereo[f * 2];
        int16_t *R = &stereo[f * 2 + 1];

        // ---- reverb (combs read/write first, feeding the delay) ----
        int32_t wet = 0;
        for (int c = 0; c < REV_COMBS; c++) {
            int in = (L[0] + R[0]) >> 1;
            int32_t bufv = rv_buf[c][rv_pos[c]];
            // comb: out = in*g + buf; buf = in + buf*fb
            int32_t acc = in + ((bufv * rvb) / 10);
            rv_buf[c][rv_pos[c]] = sat(acc);
            rv_pos[c] = (rv_pos[c] + 1) % rv_len[c];
            wet += (bufv * rvb) / 10;      // tap output scaled by wet amount
        }
        wet /= REV_COMBS;

        int32_t sl = L[0] + wet;
        int32_t sr = R[0] + wet;

        // ---- delay (fed by the reverbated signal) ----
        int32_t dr = dl_buf[0][dl_pos];
        int32_t dl = dl_buf[1][dl_pos];
        dl_buf[0][dl_pos] = sat(sr + ((dr * dfb) / 10));
        dl_buf[1][dl_pos] = sat(sl + ((dl * dfb) / 10));
        dl_pos = (dl_pos + 1) % dlen;

        // ---- final volume ----
        const uint16_t vg = VOL_GAIN[fx_volume];
        L[0] = sat(((sl + (dr * dfb) / 10) * vg) >> 8);
        R[0] = sat(((sr + (dl * dfb) / 10) * vg) >> 8);
    }
}
