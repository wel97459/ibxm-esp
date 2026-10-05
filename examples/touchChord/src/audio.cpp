// Ibmxchord audio engine: ibxm player + pipeline, track loading (flash + SD),
// chord / single-note / drum engines, song playback, menu value hooks.

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_spi_flash.h>
#include "ibxm.h"
#include "ibxm_ring.h"
#include "audio.h"
#include "ui.h"
#include "fx.h"
#include "esp_tft.h"      // TFT_SPI_HOST (SD shares the display bus)

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
const char *TRACK_NAMES[2]  = {"Space Shell", "i'm back!"};
int g_track = 0;  // which partition is currently loaded

// ---- music ----
static const uint8_t SCALE_STEPS[7]  = {0, 2, 4, 5, 7, 9, 11}; // major scale
static const int8_t  CHORD_INT[7][3] = {
  {0, 4, 7}, {0, 3, 7}, {0, 3, 7}, {0, 4, 7}, {0, 4, 7}, {0, 3, 7}, {0, 3, 6},
};
int8_t key_root = 0;   // 0..11 from C
int8_t octave   = 0;   // -2..+2
int8_t cur_inst  = 1;  // 1-based instrument all keys play

// ---- ibxm ----
struct ibxm_player *g_player = nullptr;
static ibxm_ring_t *g_ring = nullptr;
static volatile bool g_running = false;
static spi_flash_mmap_handle_t g_mmap;
static uint8_t *g_sdbuf = nullptr;      // SD track load buffer
static uint32_t g_sdlen = 0;
static bool     sd_mounted = false;
static char     sd_browse[64] = "";      // current folder (relative to card root, "" = root)
static char     sd_names[40][24];        // entry names in the browse folder
static char     sd_paths[40][96];        // full path for fopen/opendir
static bool     sd_isdir[40];            // entry is a folder (enterable)
static uint32_t sd_size[40];             // file size (bytes); 0 for folders
static int      n_sd = 0;
static TaskHandle_t g_render_task = nullptr;
static TaskHandle_t g_i2s_task = nullptr;
#define NUM_CHORD_CHANNELS 17   // all hardware channels in the pool

// channel pool: 0 = free, else key_id+1. Allocated 3-per-key on demand.
static int8_t chan_pool[NUM_CHORD_CHANNELS];

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
        fx_process(out, got);   // final-output FX (reverb + delay)
        size_t written = 0;
        i2s_write(I2S_PORT, out, (size_t)got * 2 * sizeof(int16_t),
                  &written, pdMS_TO_TICKS(I2S_WRITE_MS));
    }
    free(out);
    g_i2s_task = nullptr;
    vTaskDelete(nullptr);
}

bool init_i2s() {
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
    bool ok = i2s_set_pin(I2S_PORT, &pins) == ESP_OK;
    if (ok) fx_init(SAMPLE_RATE);
    return ok;
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
bool key_held[7];
static int  key_voice[7] = {-1,-1,-1,-1,-1,-1,-1};  // single-note: each key's own channel

// Chord-mode shared voices: at most one chord sounds at a time.
// The ROOT follows the NEWEST held key; the 3rd/5th follow the ANCHOR (oldest
// held key). Pressing an extra key while one is held moves only the root; the
// upper voices re-commit to the new chord only when the anchor is released.
static int  held_order[7];     // press order of held degrees; -1 = empty slot
static int  chord_voice_ch[4] = {-1, -1, -1, -1};  // the shared chord voices
static int  chord_commit_deg = -1;  // anchor: defines 3rd/5th
static int  chord_root_deg   = -1;  // newest held: defines root

// Ibmxchord "joystick" voicing: 4 directions recolor the held chord (transient,
// like the real device's joystick). 0=none, 1=Up(flip 3rd), 2=Right(7th),
// 3=Down(sus4), 4=Left(6th).
int active_voicing = 0;
bool single_note = false;  // toggle: true = root-only, false = full chord

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

// ---- song playback (original tracker sequencer) ----
bool song_playing = false;   // original tracker song audible?

void song_play(void) {
    if (!g_player) return;
    ibxm_sequence_play(g_player);   // unmute + restart from the top
    song_playing = true;
    Serial.println("[song] playing original");
}
void song_stop(void) {
    if (g_player) ibxm_sequence_stop(g_player);
    song_playing = false;
    Serial.println("[song] stopped");
}

// ---- drum mode ----
// Each of the 7 buttons plays its assigned instrument as a single hit
// (no chord intervals). Tuning is in semitones; 0 = middle C.
#define DRUM_BASE_NOTE 60   // ibxm key for middle C
bool   drum_mode = false;
static int8_t drum_inst[7] = {0, 1, 2, 3, 4, 5, 6};   // playable-list index per button
static int8_t drum_tune[7] = {0, 0, 0, 0, 0, 0, 0};   // semitones from middle C
static void drum_retrig(int idx);

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

void trigger_chord(int deg) {
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
        // independently. No chord-mode shared voices involved.
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

void release_chord(int deg) {
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
void apply_voicing(int v) {
    active_voicing = v;
    if (!single_note && chord_commit_deg != -1) trigger_chord_now();
}

// Re-strike whatever is currently sounding after a global change (root, octave,
// instrument, or single-note toggle). In chord mode one shared chord is
// re-committed; in single-note each held key re-triggers its own voice.
void refresh_held(void) {
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

const char *NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const char *VOICE_NAMES[9] = {"none","flip3rd","7th","sus4","6th","aug","dom7","add9","sus2"};

int dpad_v = 0;   // currently-applied voicing (0 = base triad)

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
// ring and mmap all point at the new module.
// Common post-load work: fresh ring + tasks + silent sequencer + clean chord state.
static bool start_pipeline(void) {
    g_ring = ibxm_ring_create(RING_FRAMES, SAMPLE_RATE);
    if (!g_ring) { Serial.println("ring alloc failed"); return false; }
    ibxm_sequence_stop(g_player);   // ensure new module starts silent (patterns off)
    song_playing = false;
    cur_inst = 1;   // reset to the first instrument on every track load
    memset(chan_pool, 0, sizeof(chan_pool));
    for (int k = 0; k < 7; k++) { key_held[k] = false; held_order[k] = -1; key_voice[k] = -1; }
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

bool load_track(void) {
    teardown_audio();
    if (!load_module(TRACK_LABELS[g_track])) { Serial.println("module load failed"); return false; }
    return start_pipeline();
}

// Leave menu / original-playback mode and return to clean Ibmxchord chord mode.
void reset_to_ibmxchord(void) {
    ibxm_sequence_stop(g_player);   // mute sequencer + hard-stop all channels
    memset(chan_pool, 0, sizeof(chan_pool));
    for (int k = 0; k < 7; k++) { key_held[k] = false; held_order[k] = -1; key_voice[k] = -1; }
    for (int i = 0; i < 4; i++) { chord_voice_ch[i] = -1; chord_voice_note[i] = -1; }
    chord_commit_deg = -1; chord_root_deg = -1;
    active_voicing = 0; dpad_v = 0;
}

// ---- SD card (shares the display SPI bus) ----
void mount_sd(void) {
    if (!display_ready) return;                 // SPI bus must be up first
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = (spi_host_device_t)TFT_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)SD_CS_PIN;
    slot.host_id = (spi_host_device_t)TFT_SPI_HOST;

    // --- diagnostic probe: initialize the card manually for a detailed error ---
    esp_err_t e = sdspi_host_init();
    sdspi_dev_handle_t probe = -1;
    if (e == ESP_OK || e == ESP_ERR_INVALID_STATE)
        e = sdspi_host_init_device(&slot, &probe);
    if (e != ESP_OK) {
        Serial.printf("[sd] device init failed: %s\n", esp_err_to_name(e));
        return;
    }
    host.slot = probe;                          // sdmmc talks through this device
    sdmmc_card_t probe_card;
    esp_err_t ci = sdmmc_card_init(&host, &probe_card);
    sdspi_host_remove_device(probe);
    if (ci != ESP_OK) {
        Serial.printf("[sd] CARD INIT FAILED: %s (0x%x) — check wiring/format\n",
                      esp_err_to_name(ci), ci);
        return;
    }
    Serial.printf("[sd] card detected: %s %uMB\n", probe_card.cid.name,
                  (unsigned)((uint64_t)probe_card.csd.capacity * probe_card.csd.sector_size >> 20));

    // --- real mount ---
    esp_vfs_fat_sdmmc_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 4;
    sdmmc_card_t *card = nullptr;
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &card);
    if (err == ESP_OK) {
        sd_mounted = true;
        Serial.printf("[sd] mounted /sdcard (%s)\n", card->cid.name);
    } else {
        Serial.printf("[sd] mount failed after card init: %s — card is likely not FAT32\n",
                      esp_err_to_name(err));
    }
}

// Scan the current browse folder: folders first, then track files,
// each group alphabetical.
void scan_sd(void) {
    n_sd = 0;
    if (!strcmp(sd_browse, "/flash")) {          // virtual folder: the 2 flash tracks
        for (int i = 0; i < 2; i++) {
            snprintf(sd_names[i], sizeof(sd_names[0]), "%s", TRACK_NAMES[i]);
            sd_isdir[i] = false;
            const esp_partition_t *part = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, TRACK_LABELS[i]);
            sd_size[i] = part ? part->size : 0;
        }
        n_sd = 2;
        Serial.println("[sd] 2 flash tracks");
        return;
    }
    if (!sd_browse[0]) {                         // card root: virtual "Flash Tracks" folder first
        snprintf(sd_names[0], sizeof(sd_names[0]), "Flash Tracks");
        snprintf(sd_paths[0], sizeof(sd_paths[0]), "/flash");
        sd_isdir[0] = true;
        sd_size[0] = 0;
        n_sd = 1;
    }
    if (!sd_mounted) return;                     // no card: only the virtual folder shows
    char dir[96];
    snprintf(dir, sizeof(dir), "/sdcard%s%s", sd_browse[0] ? "/" : "", sd_browse);
    DIR *d = opendir(dir);
    if (!d) {
        Serial.printf("[sdscan] opendir('%s') FAILED errno=%d\n", dir, errno);
        sd_browse[0] = 0;   // folder vanished: back to root
        scan_sd();
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && n_sd < 40) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;                       // hidden/junk
        char sub[96];
        snprintf(sub, sizeof(sub), "%s/%s", dir, n);
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (strlen(n) > 22) continue;
            // insertion sort: folders before files, alphabetical
            int i = 0;
            while (i < n_sd && strcasecmp(sd_names[i], n) < 0) i++;
            for (int j = n_sd; j > i; j--) {
                strcpy(sd_names[j], sd_names[j-1]); strcpy(sd_paths[j], sd_paths[j-1]);
                sd_isdir[j] = sd_isdir[j-1]; sd_size[j] = sd_size[j-1];
            }
            snprintf(sd_names[i], sizeof(sd_names[0]), "%s", n);
            snprintf(sd_paths[i], sizeof(sd_paths[0]), "%s", sub);
            sd_isdir[i] = true;
            sd_size[i] = 0;
            n_sd++;
        } else {
            const char *dot = strrchr(n, '.');
            bool ok = dot && (!strcasecmp(dot, ".xm") || !strcasecmp(dot, ".s3m") ||
                              !strcasecmp(dot, ".mod"));
            if (!ok) continue;
            // append after the current entries (folders land before files
            // because a later folder's sorted insert shifts everything right)
            int at = n_sd;
            for (int j = at; j > n_sd; j--) {
                strcpy(sd_names[j], sd_names[j-1]); strcpy(sd_paths[j], sd_paths[j-1]);
                sd_isdir[j] = sd_isdir[j-1]; sd_size[j] = sd_size[j-1];
            }
            snprintf(sd_names[at], sizeof(sd_names[0]), "%s", n);
            snprintf(sd_paths[at], sizeof(sd_paths[0]), "%s", sub);
            sd_isdir[at] = false;
            sd_size[at] = st.st_size;
            n_sd++;
        }
    }
    closedir(d);
    Serial.printf("[sd] %d entries in %s/\n", n_sd, sd_browse[0] ? sd_browse : "(root)");
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

// ---- instrument list hooks ----
int inst_count(void) {
    if (!g_player) return 0;
    int c = 0;
    for (int ins = 1; ins <= g_player->module->num_instruments; ins++)
        if (g_player->module->instruments[ins].num_samples > 0) c++;
    return c;
}
int inst_number(int idx) {   // 0-based playable index -> 1-based instrument number
    if (!g_player) return 1;
    int c = -1;
    for (int ins = 1; ins <= g_player->module->num_instruments; ins++) {
        if (g_player->module->instruments[ins].num_samples > 0) { if (++c == idx) return ins; }
    }
    return 1;
}

// ---- track list hooks (2 flash partitions + SD files) ----
int track_count(void) { return n_sd; }   // entries include the virtual Flash Tracks folder
void track_label(int i, char *buf, int bl) {
    if (i < 0 || i >= n_sd) { buf[0] = 0; return; }
    const int e = i;
    snprintf(buf, bl, "%s%s", sd_names[e], sd_isdir[e] ? "/" : "");
}
// Right-column info for the LOAD TRACK list: "D" for folders, size for files.
void track_info(int i, char *buf, int bl) {
    if (i < 0 || i >= n_sd) { buf[0] = 0; return; }
    const int e = i;
    if (sd_isdir[e]) { snprintf(buf, bl, "D"); return; }
    uint32_t sz = sd_size[e];
    if (sz >= (1024u*1024u))      snprintf(buf, bl, "%uM", (unsigned)((sz + (512u*1024u)) >> 20));
    else if (sz >= 1024u)         snprintf(buf, bl, "%uK", (unsigned)((sz + 512u) >> 10));
    else                          snprintf(buf, bl, "%u", (unsigned)sz);
}

// L inside LOAD TRACK: go up one folder. Returns true if consumed.
bool track_back(void) {
    if (!strcmp(sd_browse, "/flash")) {          // L: out of the virtual folder
        sd_browse[0] = 0;
        scan_sd();
        return true;
    }
    if (!sd_browse[0]) return false;
    char *slash = strrchr(sd_browse, '/');
    if (slash) *slash = 0; else sd_browse[0] = 0;
    scan_sd();
    Serial.printf("[sd] folder %s/\n", sd_browse[0] ? sd_browse : "(root)");
    return true;
}
static void show_loading(const char *name) {
    Serial.printf("[load] %s...\n", name); Serial.flush();
    ui_loading(name);
}

void track_select(int i) {
    if (!strcmp(sd_browse, "/flash")) {          // virtual folder: flash track selected
        if (i >= 0 && i < 2) {
            const int e = i;
            show_loading(sd_names[e]);
            if (g_track != e) { g_track = e; load_track(); }
            Serial.printf("[menu] track %d: %s\n", g_track + 1, TRACK_NAMES[g_track]);
            ui_loading_done();
        }
    } else if (i < n_sd) {
        const int e = i;
        if (sd_isdir[e]) {                       // enter the folder
            if (!strcmp(sd_paths[e], "/flash")) {    // virtual folder
                snprintf(sd_browse, sizeof(sd_browse), "/flash");
                scan_sd();
                Serial.println("[sd] folder Flash Tracks/");
                return;
            }
            char rel[48];
            snprintf(rel, sizeof(rel), "%s%s%s", sd_browse[0] ? "/" : "", sd_browse, sd_paths[e] + strlen("/sdcard"));
            // rebuild relative path of the entered dir from its full path
            const char *full = sd_paths[e];
            snprintf(sd_browse, sizeof(sd_browse), "%s", full + strlen("/sdcard") + 1);
            scan_sd();
            Serial.printf("[sd] folder %s/\n", sd_browse);
            return;
        }
        show_loading(sd_names[e]);
        load_sd_track(sd_paths[e]);
        ui_loading_done();
    }
}

// ---- FX menu hooks ----
int get_fxrev(int) { return fx_reverb_amt; }
void adj_fxrev(int, int d) {
    fx_reverb_amt = (fx_reverb_amt + (d > 0 ? 1 : 9)) % 10;
    Serial.printf("[menu] reverb=%d\n", fx_reverb_amt);
}
int get_fxdly(int) { return fx_delay_amt; }
void adj_fxdly(int, int d) {
    fx_delay_amt = (fx_delay_amt + (d > 0 ? 1 : 9)) % 10;
    Serial.printf("[menu] delay=%d\n", fx_delay_amt);
}
int get_fxdlyms(int) { return fx_delay_ms / 40; }        // 40..400 in 40 ms steps
void adj_fxdlyms(int, int d) {
    fx_delay_ms += (d > 0 ? 40 : -40);
    if (fx_delay_ms > 400) fx_delay_ms = 40;
    if (fx_delay_ms < 40)  fx_delay_ms = 400;
    Serial.printf("[menu] delay time=%d ms\n", fx_delay_ms);
}

int get_fxvol(int) { return fx_volume; }
void adj_fxvol(int, int d) {
    fx_volume = (fx_volume + (d > 0 ? 1 : 9)) % 10;
    Serial.printf("[menu] volume=%d\n", fx_volume);
}

// ---- menu value hooks ----

int get_root(int) { return key_root; }
void adj_root(int, int d) {
    key_root = (key_root + (d > 0 ? 1 : 11)) % 12;
    Serial.printf("[menu] root=%s\n", NOTE_NAMES[key_root]);
    for (int k = 0; k < 7; k++) if (key_held[k]) refresh_held();
}
int get_oct(int) { return octave + 2; }
void adj_oct(int, int d) {
    octave += (d > 0 ? 1 : -1);
    if (octave > 2) octave = -2;
    if (octave < -2) octave = 2;
    Serial.printf("[menu] octave=%+d\n", octave);
    for (int k = 0; k < 7; k++) if (key_held[k]) refresh_held();
}
int get_sn(int) { return single_note ? 1 : 0; }
void adj_sn(int, int d) {
    single_note = !single_note;
    Serial.printf("[menu] single_note %s\n", single_note ? "on" : "off");
    for (int k = 0; k < 7; k++) if (key_held[k]) refresh_held();
}

// ---- drum mode hooks ----
int get_drummode(int) { return drum_mode ? 1 : 0; }
void adj_drummode(int, int d) {
    drum_mode = d > 0;
    Serial.printf("[menu] drum_mode %s\n", drum_mode ? "on" : "off");
    reset_to_ibmxchord();   // silence everything; clean slate for the new mode
    Serial.flush();
}
int get_druminst(int idx) { return drum_inst[idx]; }
void adj_druminst(int idx, int d) {
    int n = inst_count();
    if (n <= 0) return;
    drum_inst[idx] = (drum_inst[idx] + (d > 0 ? 1 : n - 1)) % n;
    drum_retrig(idx);
}
void fmt_druminst(int idx, int v, char *buf, int bl) {
    (void)idx;
    int ins = inst_number(v);
    char nm[20]; nm[0] = 0;
    if (g_player) ibxm_instrument_name(g_player, ins, nm, sizeof(nm));
    snprintf(buf, bl, "%2d %s", ins, nm);
}
int get_drumtune(int idx) { return drum_tune[idx] + 12; }   // 0..24, 12 = middle C
void adj_drumtune(int idx, int d) {
    drum_tune[idx] += (d > 0 ? 1 : -1);
    if (drum_tune[idx] > 12) drum_tune[idx] = 12;
    if (drum_tune[idx] < -12) drum_tune[idx] = -12;
    drum_retrig(idx);
}
void fmt_drumtune(int idx, int v, char *buf, int bl) {
    (void)idx;
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
