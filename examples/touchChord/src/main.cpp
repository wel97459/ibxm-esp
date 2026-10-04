// Ibmxchord — 7 chord keys + 4-way D-pad, playing the REAL tracker engine
// (ibxm) directly: the S3M's instrument samples, loops, envelopes and
// volume ramps all run; the file's pattern sequencer is muted and our
// code injects notes straight into replay channels.
//
// 7 keys = scale degrees (I ii iii IV V vi vii°) — any combo always in key.
//   D-pad LEFT/RIGHT : transpose key -1/+1 semitone
//   D-pad UP/DOWN    : octave -1/+1
//
// Each key plays a 3-note chord (root, third, fifth) on 3 replay channels,
// resampled from the S3M instrument assigned to that key.

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_spi_flash.h>
#include "ibxm.h"
#include "ibxm_ring.h"
#include "esp_tft.h"       // reused ST7789 240x240 4-wire SPI driver (ESP_TFT)
#include "esp_tft_tile.h"  // tile + 4-color palette layer
#include "esp_tft_bigfont.h" // 8x8 / big font on top of the tile layer
#include "bigfont_data.c"  // tiles_bigfont[] generated from bigfont.chr
extern "C" const uint8_t gfx_tiles[];   // gui.chr tiles (24 bytes each), from gui_tiles_data.c

static void ui_task(void*);
static bool display_ready = false;

#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <dirent.h>

#define SD_CS_PIN  11   // SD card chip-select (shares SPI bus with display)
#define SD_MISO_PIN 12  // SD card DO -> bus MISO

#ifndef HC_BCLK
#define HC_BCLK 5
#endif
#ifndef HC_WS
#define HC_WS 6
#endif
#ifndef HC_DATA
#define HC_DATA 7
#endif
#ifndef I2S_PORT
#define I2S_PORT I2S_NUM_0
#endif

#define SAMPLE_RATE       44100
#define INTERPOLATION     1
#define RING_FRAMES       2048
#define I2S_DMA_BUF_COUNT 4
#define I2S_DMA_BUF_LEN   128
#define I2S_WRITE_MS      20
#define XM_PARTITION_LABEL "xm_music"
// Two tracks: slot 1 = Space Shell (S3M), slot 2 = i'm back! (XM, validated).
static const char *TRACK_LABELS[2] = {"xm_music", "xm_music2"};
static const char *TRACK_NAMES[2]  = {"Space Shell", "i'm back!"};
static int g_track = 0;  // which partition is currently loaded

// ---- buttons: 7 chord keys + 4 dpad (to GND, internal pullups) ----
#ifndef HC_KEY_PINS
#define HC_KEY_PINS {1, 2, 3, 4, 8, 10, 13}
#endif
static const uint8_t KEY_PINS[7]  = HC_KEY_PINS;;
#ifndef HC_DPAD_PINS
#define HC_DPAD_PINS {34, 35, 36, 37}
#endif
static const uint8_t DPAD_PINS[4] = HC_DPAD_PINS;; // L R U D — GPIO34-37 are input-only on S2, perfect for buttons
#ifndef HC_INST_PIN
#define HC_INST_PIN 18
#endif
static const uint8_t INST_PIN = HC_INST_PIN;                   // cycle instrument (free GPIO, not a strapping pin like GPIO0)

// ---- music ----
static const uint8_t SCALE_STEPS[7]  = {0, 2, 4, 5, 7, 9, 11}; // major scale
static const int8_t  CHORD_INT[7][3] = {
  {0, 4, 7}, {0, 3, 7}, {0, 3, 7}, {0, 4, 7}, {0, 4, 7}, {0, 3, 7}, {0, 3, 6},
};
static int8_t key_root = 0;   // 0..11 from C
static int8_t octave   = 0;   // -2..+2
static int8_t cur_inst  = 1;  // 1-based instrument all keys play
static bool menu_mode = false;  // U+D menu: listen to original track
static bool chord_menu = false; // L+R menu: root note / single-note settings

// ---- ibxm ----
static struct ibxm_player *g_player = nullptr;
static ibxm_ring_t *g_ring = nullptr;
static volatile bool g_running = false;
static spi_flash_mmap_handle_t g_mmap;
static uint8_t *g_sdbuf = nullptr;      // SD track load buffer
static uint32_t g_sdlen = 0;
static bool     sd_mounted = false;
static char     sd_names[16][24];        // track files found on the card
static char     sd_paths[16][64];
static int      n_sd = 0;
static TaskHandle_t g_render_task = nullptr;
static TaskHandle_t g_i2s_task = nullptr;
#define NUM_CHORD_CHANNELS 17   // all hardware channels in the pool

// channel pool: 0 = free, else key_id+1. Allocated 3-per-key on demand.
static int8_t chan_pool[NUM_CHORD_CHANNELS];
static int key_chans[7][4];     // resolved channels, -1 = none (4th for 7th/add9)
static int8_t key_note_key[7][3]; // the midi key sent, for re-use on release

// ---- tasks (from the working player example) ----
static void render_task(void *arg) {
    (void)arg;
    struct replay *replay = g_player->replay;
    while (g_running) {
        int n = ibxm_ring_push_tick(replay, g_ring);
        if (n == 0) vTaskDelay(pdMS_TO_TICKS(1));
        else if (n < 0) break;   // no song to end; keep rendering forever
    }
    g_render_task = nullptr;
    vTaskDelete(nullptr);
}

static void i2s_feed_task(void *arg) {
    (void)arg;
    const int pull = I2S_DMA_BUF_LEN;
    int16_t *out = (int16_t *)malloc(pull * 2 * sizeof(int16_t));
    if (!out) { g_running = false; g_render_task = nullptr; vTaskDelete(nullptr); }
    while (g_running) {
        int got = ibxm_ring_pull(g_ring, out, pull);
        if (got == 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        size_t written = 0;
        i2s_write(I2S_PORT, out, (size_t)got * 2 * sizeof(int16_t),
                  &written, pdMS_TO_TICKS(I2S_WRITE_MS));
    }
    free(out);
    g_i2s_task = nullptr;
    vTaskDelete(nullptr);
}

static bool init_i2s() {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count        = I2S_DMA_BUF_COUNT;
    cfg.dma_buf_len          = I2S_DMA_BUF_LEN;
    cfg.tx_desc_auto_clear   = true;
    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.bck_io_num   = HC_BCLK;
    pins.ws_io_num    = HC_WS;
    pins.data_out_num = HC_DATA;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    return i2s_set_pin(I2S_PORT, &pins) == ESP_OK;
}

// ---- module load ----
static bool load_module(const char *label) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, label);
    if (!part) { Serial.printf("partition %s not found\n", label); return false; }
    const void *mapped;
    if (esp_partition_mmap(part, 0, part->size, SPI_FLASH_MMAP_DATA,
                           &mapped, &g_mmap) != ESP_OK) {
        Serial.println("mmap failed"); return false;
    }
    g_player = openArray((const uint8_t *)mapped, part->size, SAMPLE_RATE, INTERPOLATION);
    if (!g_player || !g_player->module) { Serial.println("parse failed"); return false; }
    return true;
}

// ---- chord triggering ----
static bool key_held[7];
static int  key_voice[7] = {-1,-1,-1,-1,-1,-1,-1};  // single-note: each key's own channel

// Chord-mode shared voices: at most one chord sounds at a time.
// The ROOT follows the NEWEST held key; the 3rd/5th follow the ANCHOR (oldest
// held key). Pressing an extra key while one is held moves only the root; the
// upper voices re-commit to the new chord only when the anchor is released.
static int  held_order[7];     // press order of held degrees; -1 = empty slot
static int  held_count = 0;
static int  chord_voice_ch[4] = {-1, -1, -1, -1};  // the shared chord voices
static int  chord_commit_deg = -1;  // anchor: defines 3rd/5th
static int  chord_root_deg   = -1;  // newest held: defines root

// Ibmxchord "joystick" voicing: 4 directions recolor the held chord (transient,
// like the real device's joystick). 0=none, 1=Up(flip 3rd), 2=Right(7th),
// 3=Down(sus4), 4=Left(6th).
static int active_voicing = 0;
static bool single_note = false;  // L+R toggle: true = root-only, false = full chord

// Apply a voicing to the 3 triad note-keys (base = root+CHORD_INT[deg][i]).
// Returns the recolored semitone offsets (relative to chord root).
static void voicing_offsets(int deg, int v, int out[4], int *n) {
    int t[3] = {CHORD_INT[deg][0], CHORD_INT[deg][1], CHORD_INT[deg][2]}; // 0,3/4/7,7
    *n = 3;
    if (v == 1) {            // Up: flip 3rd (major<->minor)
        t[1] = (t[1] == 4) ? 3 : 4;
    } else if (v == 2) {     // Right: add 7th (maj7/min7)
        out[0]=t[0]; out[1]=t[1]; out[2]=t[2]; out[3]=(t[0]==0?0:0)+11; *n=4; return;
    } else if (v == 3) {     // Down: sus4 (replace 3rd with 4th)
        t[1] = 5;
    } else if (v == 4) {     // Left: 6th (replace 5th with 6th)
        t[2] = 9;
    } else if (v == 5) {     // Up-Left: Aug (raise 5th a semitone)
        t[2] = 8;
    } else if (v == 6) {     // Up-Right: Dom7 (flat-7 instead of maj7)
        out[0]=t[0]; out[1]=t[1]; out[2]=t[2]; out[3]=10; *n=4; return;
    } else if (v == 7) {     // Down-Right: add9 (root+14)
        out[0]=t[0]; out[1]=t[1]; out[2]=t[2]; out[3]=14; *n=4; return;
    } else if (v == 8) {     // Down-Left: sus2 (replace 3rd with 2nd)
        t[1] = 2;
    }
    out[0]=t[0]; out[1]=t[1]; out[2]=t[2];
}

// In single-note mode each of the 7 buttons plays one chord tone, spread across
// octaves. The chord tones come from the *current* (D-pad) voicing so a 7th
// voicing gives 4 tones before wrapping. btn 1 = root, 2 = 2nd tone, 3 = 3rd
// tone, 4 = root+octave, 5 = 2nd tone+octave, etc.
// Returns the semitone offset (from the chord root) for the given 1-based button.
static int single_note_key(int deg, int btn) {
    int offs[4]; int n;
    voicing_offsets(deg, active_voicing, offs, &n);  // n = 3 (triad) or 4 (7th/add9)
    if (n < 1) n = 1;
    btn = (btn < 1) ? 1 : btn;
    int idx = (btn - 1) % n;
    int oct = (btn - 1) / n;
    return offs[idx] + 12 * oct;
}

// Compute the set of note-keys for the full chord of degree `deg` at the given
// voicing. The ROOT pitch comes from `root_deg` (newest key), the 3rd/5th/etc.
// come from `commit_deg` (the anchor). Returns count in *n.
static void chord_note_keys(int root_deg, int commit_deg, int v, int out[4], int *n) {
    int root_base = 24 + key_root + octave * 12 + SCALE_STEPS[root_deg] + 12;
    int cmt_base  = 24 + key_root + octave * 12 + SCALE_STEPS[commit_deg] + 12;
    if (root_base  < 1) root_base  = 1;
    if (cmt_base   < 1) cmt_base   = 1;
    int offs[4]; int on;
    voicing_offsets(commit_deg, v, offs, &on);   // shape from the anchor
    *n = on;
    for (int i = 0; i < on; i++) {
        int base = (i == 0) ? root_base : cmt_base;   // root follows newest, rest follow anchor
        int note_key = base + offs[i];
        if (note_key > 96) note_key = 96;
        out[i] = note_key;
    }
}

// Find a free hardware channel — one not in use by the chord-mode shared voices
// (chord_voice_ch) NOR by any single-note key voice (key_voice). Returns -1 if
// none free. 17 channels >> what we use, so we never need to steal.
static int alloc_chord_voice(void) {
    for (int c = 0; c < NUM_CHORD_CHANNELS; c++) {
        bool used = false;
        for (int i = 0; i < 4; i++) if (chord_voice_ch[i] == c) used = true;
        for (int k = 0; k < 7; k++) if (key_voice[k] == c) used = true;
        if (!used) return c;
    }
    return -1;
}

// ---- drum mode state ----
// Each of the 7 buttons plays its assigned instrument as a single hit
// (no chord intervals). Tuning is in semitones; 0 = middle C.
#define DRUM_BASE_NOTE 60   // ibxm key for middle C
static bool   drum_mode = false;
static int8_t drum_inst[7] = {0, 1, 2, 3, 4, 5, 6};   // playable-list index per button
static int8_t drum_tune[7] = {0, 0, 0, 0, 0, 0, 0};   // semitones from middle C
static void drum_retrig(int idx);
static int  inst_number(int idx);

// (Re)trigger the shared chord with the current anchor/root/voicing.
// Only (re)issues ibxm_note_on for voices whose note actually changed or that are
// newly allocated — a sustained re-commit (e.g. on a release) must NOT re-attack
// an already-sounding voice, or you get a double-trigger on every release.
static int chord_voice_note[4] = {-1, -1, -1, -1};
static void trigger_chord_now(void) {
    int out[4]; int n;
    chord_note_keys(chord_root_deg, chord_commit_deg, active_voicing, out, &n);
    for (int i = 0; i < n; i++) {
        int ch = chord_voice_ch[i];
        bool fresh = false;
        if (ch < 0) { ch = alloc_chord_voice(); chord_voice_ch[i] = ch; fresh = true; }
        if (ch < 0) break;
        if (fresh || chord_voice_note[i] != out[i]) {
            ibxm_note_on(g_player, ch, out[i], cur_inst, 0x40);
            chord_voice_note[i] = out[i];
        }
    }
    // silence any surplus voices (e.g. 7th -> triad)
    for (int i = n; i < 4; i++) {
        int ch = chord_voice_ch[i];
        if (ch >= 0) { ibxm_note_off(g_player, ch); chord_voice_ch[i] = -1; chord_voice_note[i] = -1; }
    }
}

static void trigger_chord(int deg) {
    if (drum_mode) {
        // Drum mode: each button is a single hit of its assigned instrument,
        // tuned from middle C. One voice per button (key_voice[]).
        int note = DRUM_BASE_NOTE + drum_tune[deg];
        if (note > 96) note = 96;
        if (note < 1) note = 1;
        int ins = inst_number(drum_inst[deg]);
        int ch = alloc_chord_voice();
        key_voice[deg] = ch;
        if (ch >= 0) {
            ibxm_note_on(g_player, ch, note, ins, 0x40);
            Serial.printf("[drum %d] inst=%d note=%d ch=%d\n", deg + 1, ins, note, ch);
        } else {
            Serial.printf("[drum %d] no free voice\n", deg + 1);
        }
        Serial.flush();
        key_held[deg] = true;
        return;
    }
    if (single_note) {
        // Each key gets its OWN voice (tracked in key_voice[]) so it releases
        // independently. No chord_mode shared voices involved.
        int note = 24 + key_root + octave*12 + SCALE_STEPS[deg] + 12 + single_note_key(deg, deg + 1);
        if (note > 96) note = 96;
        int ch = alloc_chord_voice();   // free hardware channel (not in chord_voice_ch)
        key_voice[deg] = ch;
        if (ch >= 0) {
            ibxm_note_on(g_player, ch, note, cur_inst, 0x40);
            Serial.printf("[sn %d] note=%d ch=%d\n", deg + 1, note, ch);
        } else {
            Serial.printf("[sn %d] no free voice\n", deg + 1);
        }
        Serial.flush();
        key_held[deg] = true;
        return;
    }

    Serial.printf("[key %d down] inst=%d\n", deg + 1, cur_inst); Serial.flush();
    // record press order
    int slot = -1;
    for (int s = 0; s < 7; s++) if (held_order[s] == -1) { slot = s; break; }
    if (slot >= 0) held_order[slot] = deg;
    // anchor = oldest entry; root = newest entry
    int anchor = -1, newest = -1;
    for (int s = 0; s < 7; s++) if (held_order[s] != -1) { if (anchor == -1) anchor = held_order[s]; newest = held_order[s]; }
    bool first = (chord_commit_deg == -1);
    chord_root_deg   = newest;
    chord_commit_deg = first ? newest : anchor;
    trigger_chord_now();
    key_held[deg] = true;
}

static void release_chord(int deg) {
    if (drum_mode) {
        int ch = key_voice[deg];
        if (ch >= 0) { ibxm_note_off(g_player, ch); key_voice[deg] = -1; }
        key_held[deg] = false;
        return;
    }
    Serial.printf("[key %d up]\n", deg + 1); Serial.flush();
    if (single_note) {
        int ch = key_voice[deg];
        if (ch >= 0) { ibxm_note_off(g_player, ch); key_voice[deg] = -1; }
        key_held[deg] = false;
        return;
    }

    // remove this degree from the press order
    for (int s = 0; s < 7; s++) if (held_order[s] == deg) held_order[s] = -1;
    // recompute anchor/root from the remaining held keys
    chord_commit_deg = -1; chord_root_deg = -1;
    for (int s = 0; s < 7; s++) if (held_order[s] != -1) { if (chord_commit_deg == -1) chord_commit_deg = held_order[s]; chord_root_deg = held_order[s]; }
    if (chord_commit_deg == -1) {
        // nothing held: silence and free the shared chord voices
        for (int i = 0; i < 4; i++) {
            int ch = chord_voice_ch[i];
            if (ch >= 0) { ibxm_note_off(g_player, ch); chord_voice_ch[i] = -1; chord_voice_note[i] = -1; }
        }
    } else {
        trigger_chord_now();   // re-commit to the new anchor's chord
    }
    key_held[deg] = false;
}

// Recolor every currently-held chord when the voicing changes (D-pad).
static void apply_voicing(int v) {
    active_voicing = v;
    if (!single_note && chord_commit_deg != -1) trigger_chord_now();
}

// Re-strike whatever is currently sounding after a global change (root, octave,
// instrument, or single-note toggle). In chord mode one shared chord is
// re-committed; in single-note each held key re-triggers its own voice.
static void refresh_held(void) {
    if (single_note) {
        for (int k = 0; k < 7; k++) if (key_held[k]) {
            int note = 24 + key_root + octave*12 + SCALE_STEPS[k] + 12 + single_note_key(k, k + 1);
            if (note > 96) note = 96;
            int ch = key_voice[k];
            if (ch < 0) { ch = alloc_chord_voice(); key_voice[k] = ch; }
            if (ch >= 0) ibxm_note_on(g_player, ch, note, cur_inst, 0x40);
        }
    } else if (chord_commit_deg != -1) {
        trigger_chord_now();
    }
}

static const char *NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const char *VOICE_NAMES[9] = {"none","flip3rd","7th","sus4","6th","aug","dom7","add9","sus2"};

static int dpad_v = 0;   // currently-applied voicing (0 = base triad)

// Free everything from a previously-loaded player (module + replay + struct).
static void dispose_player(struct ibxm_player *p) {
    if (!p) return;
    if (p->replay) dispose_replay(p->replay);
    if (p->module) dispose_module(p->module);
    free(p);
}

// Tear down the running audio pipeline (tasks + ring + player + mmap) so a new
// track can be loaded. Cooperative: we flag g_running=false and let each task
// finish its current loop, null its handle, and self-delete — then we free
// shared resources only once both tasks are gone. Force-deleting a task that is
// blocked on the ring's semaphore corrupts the wait list and panics, so we never
// call vTaskDelete() on a running task here.
static void teardown_audio(void) {
    if (g_player) {
        ibxm_sequence_stop(g_player);   // mute sequencer + hard-stop all channels
    }
    g_running = false;                       // tasks exit their loops on next tick
    uint32_t t0 = millis();
    while ((g_render_task || g_i2s_task) && millis() - t0 < 1000) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    // Fallback: if a task didn't self-terminate in time, delete it (rare).
    if (g_render_task) { vTaskDelete(g_render_task); g_render_task = nullptr; }
    if (g_i2s_task)    { vTaskDelete(g_i2s_task);    g_i2s_task = nullptr; }
    if (g_ring)   { ibxm_ring_destroy(g_ring); g_ring = nullptr; }
    if (g_player) { struct ibxm_player *dead = g_player; g_player = nullptr; dispose_player(dead); }
    if (g_mmap)   { spi_flash_munmap(g_mmap);  g_mmap = 0; }
    if (g_sdbuf)  { free(g_sdbuf); g_sdbuf = nullptr; g_sdlen = 0; }
}

// Reload the currently-selected track: full pipeline restart so the render task,
// ring and mmap all point at the new module (U/D in the menu).
// Common post-load work: fresh ring + tasks + silent sequencer + clean chord state.
static bool start_pipeline(void) {
    g_ring = ibxm_ring_create(RING_FRAMES, SAMPLE_RATE);
    if (!g_ring) { Serial.println("ring alloc failed"); return false; }
    ibxm_sequence_stop(g_player);   // ensure new module starts silent (patterns off)
    cur_inst = 1;   // reset to the first instrument on every track load
    memset(chan_pool, 0, sizeof(chan_pool));
    memset(key_chans, -1, sizeof(key_chans));
    for (int k = 0; k < 7; k++) { key_held[k] = false; held_order[k] = -1; key_voice[k] = -1; }
    held_count = 0;
    for (int i = 0; i < 4; i++) { chord_voice_ch[i] = -1; chord_voice_note[i] = -1; }
    chord_commit_deg = -1; chord_root_deg = -1;
    active_voicing = 0; dpad_v = 0;
    g_running = true;
    // NOTE: ui_task + btn_task are created ONCE in setup() and keep running
    // across track switches — recreating them here would leak tasks.
    xTaskCreatePinnedToCore(render_task, "render", 4096, nullptr, 5, &g_render_task, 1);
    xTaskCreatePinnedToCore(i2s_feed_task, "i2s", 4096, nullptr, 5, &g_i2s_task, 1);
    return true;
}

static bool load_track(void) {
    teardown_audio();
    if (!load_module(TRACK_LABELS[g_track])) { Serial.println("module load failed"); return false; }
    return start_pipeline();
}

// Leave menu / original-playback mode and return to clean Ibmxchord chord mode.
static void reset_to_ibmxchord(void) {
    ibxm_sequence_stop(g_player);   // mute sequencer + hard-stop all channels
    memset(chan_pool, 0, sizeof(chan_pool));
    memset(key_chans, -1, sizeof(key_chans));
    for (int k = 0; k < 7; k++) { key_held[k] = false; held_order[k] = -1; key_voice[k] = -1; }
    held_count = 0;
    for (int i = 0; i < 4; i++) { chord_voice_ch[i] = -1; chord_voice_note[i] = -1; }
    chord_commit_deg = -1; chord_root_deg = -1;
    active_voicing = 0; dpad_v = 0;
}

static void dpad_action(int d) {
    int v = (d == 0) ? 4 : (d == 1) ? 2 : (d == 2) ? 1 : 3;
    apply_voicing(v);
    Serial.printf("[voicing %s]\n", VOICE_NAMES[v]);
    Serial.flush();
}

// ---- SD card (shares the display SPI bus) ----
static void mount_sd(void) {
    if (!display_ready) return;                 // SPI bus must be up first
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = (spi_host_device_t)TFT_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)SD_CS_PIN;
    slot.host_id = (spi_host_device_t)TFT_SPI_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 4;
    sdmmc_card_t *card = nullptr;
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &card);
    if (err == ESP_OK) {
        sd_mounted = true;
        Serial.printf("[sd] mounted /sdcard (%s, %02x:%02x)\n", card->cid.name,
                      (unsigned)card->csd.capacity >> 16, 0u);
    } else {
        Serial.printf("[sd] mount failed: %s (no card inserted?)\n", esp_err_to_name(err));
    }
}

static void scan_sd(void) {
    n_sd = 0;
    if (!sd_mounted) return;
    DIR *d = opendir("/sdcard");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && n_sd < 16) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        bool ok = len > 4 && len < 23 &&
            (!strcasecmp(n + len - 3, ".xm") || !strcasecmp(n + len - 3, ".s3m") ||
             !strcasecmp(n + len - 3, ".mod"));
        if (!ok) continue;
        snprintf(sd_names[n_sd], sizeof(sd_names[0]), "%s", n);
        snprintf(sd_paths[n_sd], sizeof(sd_paths[0]), "/sdcard/%s", n);
        n_sd++;
    }
    closedir(d);
    Serial.printf("[sd] %d track file(s)\n", n_sd);
}

// Load a module file from the SD card into PSRAM and play it.
static bool load_sd_track(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { Serial.printf("[sd] open failed: %s\n", path); return false; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024*1024) { fclose(fp); Serial.println("[sd] bad size"); return false; }
    uint8_t *buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(fp); Serial.println("[sd] OOM"); return false; }
    size_t rd = fread(buf, 1, sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) { free(buf); Serial.println("[sd] short read"); return false; }
    // old buffer is freed by teardown
    teardown_audio();
    g_player = openArray(buf, (uint32_t)sz, SAMPLE_RATE, 1);
    g_sdbuf = buf; g_sdlen = sz;   // kept for the player's lifetime, freed at next teardown
    if (!g_player) { Serial.println("[sd] parse failed"); return false; }
    if (!start_pipeline()) return false;
    Serial.printf("[sd] playing %s (%ld bytes)\n", path, sz);
    return true;
}

// ================= reusable menu object =================
// A menu is a table of MenuItems. Types:
//   MI_ACTION  — running a command (R fires it)
//   MI_VALUE   — get()/adjust(±1) pair; L/R changes the value, shown after the label
//   MI_SUBMENU — pushes another Menu (R enters, L goes back)
//   MI_INSTLIST / MI_TRACKLIST — dynamic lists rendered with list_count/list_label
enum MIType : uint8_t { MI_ACTION, MI_VALUE, MI_SUBMENU, MI_INSTLIST, MI_TRACKLIST };
struct MenuItem;
struct Menu;
struct MenuItem {
    const char *label;
    MIType      type;
    int         (*get)(int idx);        // MI_VALUE: current value (idx = item context, e.g. drum button)
    const char **vals;                 // MI_VALUE: optional value names
    int         nvals;
    void        (*adjust)(int idx, int d);  // MI_VALUE: d = +1 / -1
    const Menu *sub;               // MI_SUBMENU
    void        (*act)(void);      // MI_ACTION
    int         idx;               // context passed to get/adjust (0 for plain items)
    void        (*fmt)(int idx, int v, char *buf, int bl);  // optional custom value text
};
struct Menu { const char *title; const MenuItem *items; int n; };

static bool        menu_open = false;
static int         menu_sel  = 0;
static const Menu *menu_cur  = nullptr;
static const Menu *menu_stack[4];
static int         menu_sp   = 0;

static void menu_close(void) {
    menu_open = false; menu_cur = nullptr; menu_sp = 0;
    if (dpad_v != 0) { apply_voicing(0); dpad_v = 0; }   // clean base triad on return
    Serial.println("[menu] closed"); Serial.flush();
}
static void menu_open_at(const struct Menu *m) {
    if (!menu_open) { menu_open = true; menu_sp = 0; }
    else if (menu_sp < 4 && menu_cur) menu_stack[menu_sp++] = menu_cur;
    menu_cur = m; menu_sel = 0;
    Serial.printf("[menu] %s\n", m->title); Serial.flush();
}
static void menu_back(void) {
    if (menu_sp > 0) { menu_cur = menu_stack[--menu_sp]; menu_sel = 0; }
    else menu_close();
}

// ---- value hooks (root / octave / voicing / single-note) ----
static int get_root(int) { return key_root; }
static void adj_root(int, int d) {
    key_root = (key_root + (d > 0 ? 1 : 11)) % 12;
    Serial.printf("[menu] root=%s\n", NOTE_NAMES[key_root]);
    for (int k = 0; k < 7; k++) if (key_held[k]) refresh_held();
}
static int get_oct(int) { return octave + 2; }
static void adj_oct(int, int d) {
    octave += (d > 0 ? 1 : -1);
    if (octave > 2) octave = -2;
    if (octave < -2) octave = 2;
    Serial.printf("[menu] octave=%+d\n", octave);
    for (int k = 0; k < 7; k++) if (key_held[k]) refresh_held();
}
static int get_voic(void) { return active_voicing; }
static void adj_voic(int d) {
    int v = (active_voicing + (d > 0 ? 1 : 8)) % 9;
    apply_voicing(v); dpad_v = v;
    Serial.printf("[menu] voicing=%s\n", VOICE_NAMES[v]);
}
static int get_sn(int) { return single_note ? 1 : 0; }
static void adj_sn(int, int d) {
    single_note = !single_note;
    Serial.printf("[menu] single_note %s\n", single_note ? "on" : "off");
    for (int k = 0; k < 7; k++) if (key_held[k]) refresh_held();
}

static const char *VOIC_VALS[9] = { VOICE_NAMES[0], VOICE_NAMES[1], VOICE_NAMES[2], VOICE_NAMES[3],
    VOICE_NAMES[4], VOICE_NAMES[5], VOICE_NAMES[6], VOICE_NAMES[7], VOICE_NAMES[8] };
static const char *SN_VALS[2]   = { "off", "on" };

// ---- instrument list hooks ----
static int inst_count(void) { return g_player ? g_player->module->num_playable : 0; }
static int inst_number(int idx) {   // 0-based playable index -> 1-based instrument number
    if (!g_player) return 1;
    int c = -1;
    for (int ins = 1; ins <= g_player->module->num_instruments; ins++) {
        if (g_player->module->instruments[ins].num_samples > 0) { if (++c == idx) return ins; }
    }
    return 1;
}
static int get_curi(void) {         // where does cur_inst sit in the playable list?
    if (!g_player) return 0;
    for (int i = 0; i < inst_count(); i++) if (inst_number(i) == cur_inst) return i;
    return 0;
}

// ---- track list hooks (2 flash partitions + SD files) ----
static int track_count(void) { return 2 + n_sd; }
static void track_label(int i, char *buf, int bl) {
    if (i < 2) snprintf(buf, bl, "%s", TRACK_NAMES[i]);
    else       snprintf(buf, bl, "%s", sd_names[i - 2]);
}
static void track_select(int i) {
    if (i < 2) {
        if (g_track != i) { g_track = i; load_track(); }
        Serial.printf("[menu] track %d: %s\n", g_track + 1, TRACK_NAMES[g_track]);
    } else {
        load_sd_track(sd_paths[i - 2]);
    }
}

// ---- drum mode hooks ----
static const char *DRUM_VALS[2] = { "off", "on" };
static int get_drummode(int) { return drum_mode ? 1 : 0; }
static void adj_drummode(int, int d) {
    drum_mode = d > 0;
    Serial.printf("[menu] drum_mode %s\n", drum_mode ? "on" : "off");
    reset_to_ibmxchord();   // silence everything; clean slate for the new mode
    Serial.flush();
}
static int get_druminst(int idx) { return drum_inst[idx]; }
static void adj_druminst(int idx, int d) {
    int n = inst_count();
    if (n <= 0) return;
    drum_inst[idx] = (drum_inst[idx] + (d > 0 ? 1 : n - 1)) % n;
    drum_retrig(idx);
}
static void fmt_druminst(int idx, int v, char *buf, int bl) {
    int ins = inst_number(v);
    char nm[20]; nm[0] = 0;
    if (g_player) ibxm_instrument_name(g_player, ins, nm, sizeof(nm));
    snprintf(buf, bl, "%2d %s", ins, nm);
}
static int get_drumtune(int idx) { return drum_tune[idx] + 12; }   // 0..24, 12 = middle C
static void adj_drumtune(int idx, int d) {
    drum_tune[idx] += (d > 0 ? 1 : -1);
    if (drum_tune[idx] > 12) drum_tune[idx] = 12;
    if (drum_tune[idx] < -12) drum_tune[idx] = -12;
    drum_retrig(idx);
}
static void fmt_drumtune(int idx, int v, char *buf, int bl) {
    int t = v - 12;
    if (t == 0) snprintf(buf, bl, "C4 (middle C)");
    else snprintf(buf, bl, "C4 %+d st", t);
}
// re-trigger a held drum button so edits are heard live
static void drum_retrig(int idx) {
    if (!drum_mode || !key_held[idx] || !g_player) return;
    int ch = key_voice[idx];
    if (ch >= 0) ibxm_note_off(g_player, ch);
    int note = DRUM_BASE_NOTE + drum_tune[idx];
    if (note > 96) note = 96;
    if (note < 1) note = 1;
    int ins = inst_number(drum_inst[idx]);
    ibxm_note_on(g_player, ch, note, ins, 0x40);
}

extern const Menu MENU_DRUMS;   // defined below (referenced by the Sound submenu)

static const MenuItem ITEMS_SOUND[] = {
    { "Single Note", MI_VALUE, get_sn,   SN_VALS,   2, adj_sn,   nullptr, nullptr, 0, nullptr },
    { "Drum Mode",   MI_VALUE, get_drummode, DRUM_VALS, 2, adj_drummode, nullptr, nullptr, 0, nullptr },
    { "Drums",       MI_SUBMENU, nullptr, nullptr, 0, nullptr, &MENU_DRUMS, nullptr, 0, nullptr },
    { "Back",        MI_ACTION, nullptr, nullptr, 0, nullptr, nullptr, [](){ menu_back(); }, 0, nullptr },
};
static const Menu MENU_SOUND = { "SOUND", ITEMS_SOUND, 4 };

static const Menu MENU_INST  = { "INSTRUMENT", nullptr, 0 };  // dynamic (MI_INSTLIST)
static const Menu MENU_TRACK = { "LOAD TRACK", nullptr, 0 };  // dynamic (MI_TRACKLIST)

static const MenuItem ITEMS_ROOT[] = {
    { "Instrument", MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_INST, nullptr },
    { "Sound",      MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_SOUND, nullptr },
    { "Root",       MI_VALUE,    get_root, NOTE_NAMES, 12, adj_root, nullptr, nullptr },
    { "Octave",     MI_VALUE,    get_oct,  nullptr, 0, adj_oct, nullptr, nullptr },
    { "Load Track", MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_TRACK, nullptr },
    { "Close",      MI_ACTION,   nullptr, nullptr, 0, nullptr, nullptr, [](){ menu_close(); } },
};
static const Menu MENU_ROOT = { "IBMXCHORD MENU", ITEMS_ROOT, 6 };

MenuItem ITEMS_DRUMS[15];   // 7 x (inst, tune) + Back - built in menu_init()
const Menu MENU_DRUMS = { "DRUMS", ITEMS_DRUMS, 15 };

// Build the per-button drum rows: Btn k = {Inst, Tune}, then a Back row.
static void menu_init(void) {
    static const char *BTN[7] = { "Key 1", "Key 2", "Key 3", "Key 4", "Key 5", "Key 6", "Key 7" };
    for (int k = 0; k < 7; k++) {
        ITEMS_DRUMS[k*2]   = { BTN[k],           MI_VALUE, get_druminst, nullptr, 0, adj_druminst, nullptr, nullptr, k, fmt_druminst };
        ITEMS_DRUMS[k*2+1] = { "   tuning",      MI_VALUE, get_drumtune, nullptr, 0, adj_drumtune, nullptr, nullptr, k, fmt_drumtune };
    }
    ITEMS_DRUMS[14] = { "Back", MI_ACTION, nullptr, nullptr, 0, nullptr, nullptr, [](){ menu_back(); }, 0, nullptr };
}

// ---- menu input: U/D cursor, L/R value-or-nav, U+D/L+R quick access ----
static void menu_nav(bool up, bool rt, bool dn, bool lf) {
    static bool u_was=false, d_was=false, r_was=false, l_was=false, q_ud=false, q_lr=false;
    bool is_list = (menu_cur->items == nullptr) ||
                   (menu_cur->items[menu_sel].type == MI_INSTLIST) ||
                   (menu_cur->items[menu_sel].type == MI_TRACKLIST);

    if (is_list) {
        int n = (menu_cur == &MENU_INST) ? inst_count() : track_count();
        if (n > 0) {
                if (up && !dn && !u_was) menu_sel = (menu_sel + n - 1) % n;
            if (dn && !up && !d_was) menu_sel = (menu_sel + 1) % n;
            if (lf && !l_was) menu_back();   // L = back in these list submenus
            if (rt && !r_was) {
                if (menu_cur == &MENU_INST) {
                    cur_inst = inst_number(menu_sel);
                    char nm[32]; ibxm_instrument_name(g_player, cur_inst, nm, sizeof(nm));
                    Serial.printf("[menu] inst %d: %s\n", cur_inst, nm);
                } else {
                    track_select(menu_sel);
                }
            }
        }
    } else {
        const MenuItem &it = menu_cur->items[menu_sel];
        int n = menu_cur->n;
        if (up && !dn && !u_was) menu_sel = (menu_sel + n - 1) % n;
        if (dn && !up && !d_was) menu_sel = (menu_sel + 1) % n;
        if (rt && !r_was) {
            switch (it.type) {
                case MI_VALUE:   if (it.adjust) it.adjust(it.idx, +1); break;
                case MI_SUBMENU: menu_open_at(it.sub); break;
                case MI_ACTION:  if (it.act) it.act(); break;
                default: break;
            }
        }
        if (lf && !l_was) {
            if (it.type == MI_VALUE) { if (it.adjust) it.adjust(it.idx, -1); }
            else menu_back();
        }
    }
    // quick access (works from anywhere in the menu)
    if (up && dn && !q_ud) menu_open_at(&MENU_INST);
    if (lf && rt && !q_lr) menu_open_at(&MENU_SOUND);
    q_ud = up && dn; q_lr = lf && rt;
    u_was = up && !dn; d_was = dn && !up; r_was = rt && !lf; l_was = lf && !rt;
}

static void scan_buttons() {
    // edge-latch state (declared once, reused by every state block below)
    static int  inst_was = 1;
    static bool ud_was = false, lr_was = false;

    // ---- 7 chord keys: always active, never close the menu ----
    for (int k = 0; k < 7; k++) {
        bool down = digitalRead(KEY_PINS[k]) == LOW;
        if (down && !key_held[k]) trigger_chord(k);
        else if (!down && key_held[k]) release_chord(k);
    }

    // ---- instrument button = menu toggle ----
    int ilevel = digitalRead(INST_PIN);
    if (ilevel != inst_was) {
        if (ilevel == LOW) {
            if (!menu_open) menu_open_at(&MENU_ROOT);
            else menu_close();
        }
        inst_was = ilevel;
    }

    // ---- D-pad ----
    bool up = digitalRead(DPAD_PINS[2]) == LOW;
    bool rt = digitalRead(DPAD_PINS[1]) == LOW;
    bool dn = digitalRead(DPAD_PINS[3]) == LOW;
    bool lf = digitalRead(DPAD_PINS[0]) == LOW;

    if (menu_open) {
        menu_nav(up, rt, dn, lf);
        return;
    }

    // closed-state quick access: U+D -> instruments, L+R -> sounding options
    if (up && dn) {
        if (!ud_was) { menu_open_at(&MENU_INST); ud_was = true; }
        return;
    }
    ud_was = false;
    if (lf && rt) {
        if (!lr_was) { menu_open_at(&MENU_SOUND); lr_was = true; }
        return;
    }
    lr_was = false;

    // ---- voicing (8 directions via diagonal synthesis) ----
    int v = 0;
    if (up && !rt && !dn && !lf) v = 1;
    else if (!up && rt && !dn && !lf) v = 2;
    else if (!up && !rt && dn && !lf) v = 3;
    else if (!up && !rt && !dn && lf) v = 4;
    else if (up && lf)  v = 5;   // Up-Left  : Aug
    else if (up && rt)  v = 6;   // Up-Right : Dom7
    else if (dn && rt)  v = 7;   // Down-Right: add9
    else if (dn && lf)  v = 8;   // Down-Left : sus2
    if (v != dpad_v) {
        apply_voicing(v);
        Serial.printf("[voicing %s]\n", VOICE_NAMES[v]);
        Serial.flush();
        dpad_v = v;
    }
}

void draw_screen_border(){
    tft_tile_putWindowGFX(0, 0, 0 | FILP_H_TILE, &gfx_tiles[0x12*0x18]);
    tft_tile_putWindowGFX(0, 29, 0 | FILP_H_TILE | FILP_V_TILE , &gfx_tiles[0x12*0x18]);
    tft_tile_putWindowGFX(29, 0, 0, &gfx_tiles[0x12*0x18]);
    tft_tile_putWindowGFX(29, 29, 0 | FILP_V_TILE, &gfx_tiles[0x12*0x18]);

    for (size_t i = 1; i < 29; i++) {
        tft_tile_putWindowGFX(i, 0, 0, &gfx_tiles[0x11*0x18]);
        tft_tile_putWindowGFX(i, 29, 0, &gfx_tiles[0x11*0x18]);
        tft_tile_putWindowGFX(0, i, 0, &gfx_tiles[0x0f*0x18]);
        tft_tile_putWindowGFX(29, i, 0, &gfx_tiles[0x0f*0x18]);
    }
}

void draw_background_pat(){
    for (size_t iy = 0; iy < 60; iy++) {
        for (size_t ix = 0; ix < 60; ix++) {
            if(iy&0x01){
                if(ix&0x01){
                    tft_tile_putGFX(ix, iy, 1, &gfx_tiles[15<<4]);
                }else{
                    tft_tile_putGFX(ix, iy, 1, &gfx_tiles[17<<4]);
                }
            } else {
                if(ix&0x01){
                    tft_tile_putGFX(ix, iy, 1, &gfx_tiles[16<<4]);
                }else{
                    tft_tile_putGFX(ix, iy, 1, &gfx_tiles[18<<4]);
                }
            }
        }
    }
}

void drawGrid() {
    for (size_t y = 0; y < 30; y++) {
        for (size_t x = 0; x < 30; x++) {
            tft_tile_putBackgoundGFX(x, y, 0, &gfx_tiles[0x13*0x18]);
        }
    }
}


// ---- ST7789 240x240 display (reused ESP_TFT + tile + bigfont) ----
static void display_init() {
    display_ready = tft_init(LCD_TYPE_ST7789, DIS_TILE_WIDTH, DIS_TILE_HEIGHT);
    if (!display_ready) {
        Serial.println("[display] init failed — running without screen");
        return;
    }
    // Tile engine + the exact palette setup from the original ESP_TFT main.c
    tft_tile_init();
    tft_tile_putPalette(0, TFT_BLACK, TFT_BLACK, tft_color565(0x28, 0x38, 0x88), TFT_RED);
    tft_tile_putPalette(1, tft_color565(0x28, 0x38, 0x88), tft_color565(0xF8, 0xF8, 0xF8), tft_color565(0xB8, 0xB8, 0xB8), tft_color565(0x60, 0x60, 0x60));
    tft_tile_putPalette(2, tft_color565(0x00, 0x00, 0x00), tft_color565(0x88, 0xe8, 0x10), tft_color565(0x00, 0x00, 0xb0), tft_color565(0x00, 0x00, 0x68));
    tft_tile_putPalette(3, tft_color565(0x00, 0x00, 0x80), tft_color565(0x00, 0x00, 0xd8), tft_color565(0x00, 0x00, 0x90), tft_color565(0x00, 0x00, 0x48));

    draw_screen_border();
    drawGrid();

    Serial.println("[display] ESP_TFT ready");
}

// Transparent-text palette helper: TRANSPARENT_TILE (0x20) makes glyph
// background pixels (b==0) keep the layer below.
#define TXT_PAL(p) ((p) | TRANSPARENT_TILE)

// Render the active menu onto the main tile layer (over the BG grid).
static void draw_menu() {
    if (!display_ready || !menu_open || !menu_cur) return;
    tft_tile_clear();
    tft_lilfont_printf(1, 1, TXT_PAL(0), "%s", menu_cur->title);

    bool is_list = (menu_cur->items == nullptr) ||
                   (menu_cur->items[menu_sel].type == MI_INSTLIST) ||
                   (menu_cur->items[menu_sel].type == MI_TRACKLIST);
    int n = is_list ? ((menu_cur == &MENU_INST) ? inst_count() : track_count())
                    : menu_cur->n;
    char line[40];
    const int VIS = 12;                       // items visible below the title
    int base = (menu_sel > VIS - 1) ? menu_sel - (VIS - 1) : 0;
    for (int row = 0; row < VIS && (base + row) < n; row++) {
        int idx = base + row;
        uint8_t y = 3 + row * 2;
        const char *cursor = (idx == menu_sel) ? "-" : " ";   // font has no '>' glyph
        if (is_list) {
            uint8_t rp = (idx == menu_sel) ? 1 : 0;   // highlight the selected row
            if (menu_cur == &MENU_INST) {
                int ins = inst_number(idx);
                char nm[28]; nm[0] = 0;
                if (g_player) ibxm_instrument_name(g_player, ins, nm, sizeof(nm));
                tft_lilfont_printf(1, y, TXT_PAL(rp), "%s%2d %s", cursor, ins, nm);
            } else {
                char lbl[24]; track_label(idx, lbl, sizeof(lbl));
                tft_lilfont_printf(1, y, TXT_PAL(rp), "%s%s", cursor, lbl);
            }
        } else {
            const MenuItem &it = menu_cur->items[idx];
            if (it.type == MI_VALUE && it.get) {
                int v = it.get(it.idx);
                char vbuf[32];
                if (it.fmt) {
                    it.fmt(it.idx, v, vbuf, sizeof(vbuf));   // custom value text (e.g. instrument names)
                } else {
                    const char *vs = (it.vals && v >= 0 && v < it.nvals) ? it.vals[v] : nullptr;
                    if (vs) snprintf(vbuf, sizeof(vbuf), "%s", vs);
                    else    snprintf(vbuf, sizeof(vbuf), "%+d", v - (it.get == get_oct ? 2 : 0));
                }
                snprintf(line, sizeof(line), "%s%s: %s", cursor, it.label, vbuf);
            } else {
                snprintf(line, sizeof(line), "%s%s", cursor, it.label);
            }
            tft_lilfont_printf(1, y, TXT_PAL(idx == menu_sel ? 1 : 0), "%s", line);
        }
    }

    // Push the composed menu frame to the panel (same as the status page).
    for (uint8_t y = 0; y < 30; y++) tft_tile_render(y, 0, 0);
    tft_tile_sendLine(29*8, 0);
}

// Mirror live state to the screen via the tile engine. Throttled ~8 fps.
// Tile coords are 8px; tft_lilfont_printf(x,y,pal,fmt...) writes at tile(x,y).
static uint32_t ui_t0 = 0;
static void draw_ui() {
    if (!display_ready) return;
    if (millis() - ui_t0 < 120) return;   // ~8 fps refresh
    ui_t0 = millis();

    if (menu_open) { draw_menu(); return; }   // menu replaces the status page

    tft_tile_clear();   // clears the MAIN map only — BG grid (drawGrid) persists

    // Title / track (palette 1 = light-on-blue)
    tft_lilfont_printf(1, 1, TXT_PAL(0), "Ibmxchord  %s", TRACK_NAMES[g_track]);

    // Key root + octave
    tft_lilfont_printf(1, 10, TXT_PAL(0), "root %s  oct %+d", NOTE_NAMES[key_root], octave);

    // Current instrument
    char iname[32] = {0};
    if (g_player) ibxm_instrument_name(g_player, cur_inst, iname, sizeof(iname));
    tft_lilfont_printf(1, 13, TXT_PAL(0), "inst %2d %s", cur_inst, iname);

    // Active voicing
    tft_lilfont_printf(1, 16, TXT_PAL(0), "voic %s", VOICE_NAMES[active_voicing]);

    // Mode line
    tft_lilfont_printf(1, 19, TXT_PAL(0),
                       drum_mode ? "DRUM MODE" : single_note ? "SINGLE-NOTE" : "CHORD MODE");

    // Held keys as a row of 7 markers (I ii iii IV V vi vii) — highlight when held.
    const char *DEG[7] = {"I","ii","iii","IV","V","vi","vii"};
    for (int k = 0; k < 7; k++) {
        uint8_t pal = key_held[k] ? 0 : 0;   // palette 0 = default
        tft_lilfont_printf(k * 4 + 1, 23, TXT_PAL(pal), "%s", DEG[k]);
    }

    // Footer hint (stays on row 29 — shifting it down one more tile would
    // push it off the 30-row screen)
    tft_lilfont_printf(1, 29, TXT_PAL(0), "L+R menu  U+D track  INST=inst/8va");

    // Push the tile map to the panel (one full 240x240 frame).
    for (uint8_t y = 0; y < 30; y++) tft_tile_render(y, 0, 0);
    tft_tile_sendLine(29*8, 0);   // band 29 is composed by the y=29 pass but never sent (each pass sends the previous band)
}

// UI runs on core 0 so display redraw never competes with the audio
// pipeline (render + i2s tasks are pinned to core 1).
static void ui_task(void*) {
    for (;;) {
        draw_ui();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Button scanning lives on the audio core (core 1): it's a few digitalReads
// per pass, runs at low priority so audio render always preempts it, and it
// no longer stalls while the core-0 draw_ui blocks on SPI line transmission.
static void btn_task(void*) {
    for (;;) {
        scan_buttons();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void setup() {
    menu_init();
    Serial.begin(115200);
    for (int k = 0; k < 7; k++) { pinMode(KEY_PINS[k], INPUT_PULLUP); key_held[k] = false; }
    for (int d = 0; d < 4; d++) pinMode(DPAD_PINS[d], INPUT_PULLUP);
    pinMode(INST_PIN, INPUT_PULLUP);

    vTaskDelay(5000 / portTICK_RATE_MS);
    Serial.println("[ibmxchord] booting..."); Serial.flush();
    if (!psramFound()) Serial.println("WARN: no PSRAM");

    display_init();
    mount_sd();
    scan_sd();

    if (!init_i2s()) { Serial.println("i2s init failed"); while (1) delay(1000); }
    g_ring = ibxm_ring_create(RING_FRAMES, SAMPLE_RATE);
    if (!g_ring) { Serial.println("ring alloc failed"); while (1) delay(1000); }

    if (!load_module(TRACK_LABELS[g_track])) { Serial.println("module load failed"); while (1) delay(1000); }
    Serial.printf("[track %d] %s\n", g_track + 1, TRACK_NAMES[g_track]); Serial.flush();

    ibxm_sequence_stop(g_player);   // mute sequencer + hard-stop all channels at boot
    memset(chan_pool, 0, sizeof(chan_pool));
    memset(key_chans, -1, sizeof(key_chans));

    g_running = true;
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(btn_task, "btn", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(render_task, "render", 4096, nullptr, 5, &g_render_task, 1);
    xTaskCreatePinnedToCore(i2s_feed_task, "i2s", 4096, nullptr, 5, &g_i2s_task, 1);

    Serial.printf("[ibmxchord ready] '%s' %d ch, inst=1/%d (of %d hdr), key=C oct=0\n",
                  g_player->module->name, g_player->module->num_channels,
                  g_player->module->num_playable, g_player->module->num_instruments);
    Serial.flush();
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }   // unused: UI runs in ui_task (core 0)
