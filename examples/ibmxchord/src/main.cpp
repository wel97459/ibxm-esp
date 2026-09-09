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
static const uint8_t KEY_PINS[7]  = {1, 2, 3, 4, 8, 10, 13};
static const uint8_t DPAD_PINS[4] = {14, 15, 16, 17}; // L R U D
static const uint8_t INST_PIN = 18;                   // cycle instrument (free GPIO, not a strapping pin like GPIO0)

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
    if (g_player) { dispose_player(g_player);  g_player = nullptr; }
    if (g_mmap)   { spi_flash_munmap(g_mmap);  g_mmap = 0; }
}

// Reload the currently-selected track: full pipeline restart so the render task,
// ring and mmap all point at the new module (U/D in the menu).
static bool load_track(void) {
    teardown_audio();
    if (!load_module(TRACK_LABELS[g_track])) { Serial.println("module load failed"); return false; }
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
    xTaskCreatePinnedToCore(render_task, "render", 4096, nullptr, 5, &g_render_task, 1);
    xTaskCreatePinnedToCore(i2s_feed_task, "i2s", 4096, nullptr, 5, &g_i2s_task, 1);
    return true;
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

static void scan_buttons() {
    // edge-latch state (declared once, reused by every menu/state block below)
    static int  inst_was = 1;
    static uint32_t inst_press_t = 0;
    static bool lr_was    = false;   // L+R together -> toggle chord_menu
    static bool ud_was    = false;   // U+D together -> toggle track menu
    static bool m_l_was=false, m_r_was=false, m_u_was=false, m_d_was=false; // track-menu edges
    static bool c_u_was=false, c_d_was=false, c_l_was=false;                 // chord-menu edges
    for (int k = 0; k < 7; k++) {
        bool down = digitalRead(KEY_PINS[k]) == LOW;
        if (down && !key_held[k]) {
            if (menu_mode || chord_menu) { menu_mode=false; chord_menu=false; reset_to_ibmxchord(); Serial.printf("[menu] exit\n"); Serial.flush(); }
            trigger_chord(k);
        }
        else if (!down && key_held[k]) release_chord(k);
    }
    // instrument select on GPIO0: a LOW edge (press) = next instrument.
    // inst_was tracks the PREVIOUS level so we fire once per press, not per scan.
    int ilevel = digitalRead(INST_PIN);
    if (ilevel != inst_was) {
        Serial.printf("[gpio18 raw=%d]\n", ilevel); Serial.flush();
        if (ilevel == LOW) inst_press_t = millis();
        else {
            if (menu_mode || chord_menu) { menu_mode=false; chord_menu=false; reset_to_ibmxchord(); Serial.printf("[menu] exit\n"); Serial.flush(); }
            struct module *m = g_player->module;
            if (millis() - inst_press_t < 600) {
                // step to the next instrument that actually has a decoded wave
                cur_inst = ibxm_next_instrument(g_player, cur_inst);
                char iname[32];
                ibxm_instrument_name(g_player, cur_inst, iname, sizeof(iname));
                Serial.printf("[instrument %d] %s\n", cur_inst, iname);
            } else {
                octave = (octave < 2) ? octave + 1 : -2;
                Serial.printf("[octave %+d]\n", octave);
            }
            for (int k = 0; k < 7; k++) if (key_held[k]) refresh_held();
            Serial.flush();
        }
        inst_was = ilevel;
    }

    // Ibmxchord joystick = 8 directions. D-pad is 4-way, so we read the X/Y axes
    // orthogonally and synthesize diagonals from held pairs (U+L=Up-Left, etc).
    // dir map (0..8): 0=none,1=Up,2=Right,3=Down,4=Left,5=Up-Left,6=Up-Right,
    //                 7=Down-Right,8=Down-Left
    bool up = digitalRead(DPAD_PINS[2]) == LOW;
    bool rt = digitalRead(DPAD_PINS[1]) == LOW;
    bool dn = digitalRead(DPAD_PINS[3]) == LOW;
    bool lf = digitalRead(DPAD_PINS[0]) == LOW;

    // L+R held together = toggle the chord-menu (root note / single-note settings).
    if (lf && rt) {
        if (!lr_was) {
            chord_menu = !chord_menu;
            if (chord_menu) Serial.printf("[cmenu] U/D=root  L=single-note (key/inst exits)\n");
            else           Serial.printf("[cmenu] exit\n");
            Serial.flush();
            lr_was = true;
        }
        return;   // don't also fire a diagonal voicing on this scan
    }
    lr_was = false;   // released (or not held): re-arm the toggle for next press
    // U+D held together = toggle the "listen to original track" menu.
    if (up && dn) {
        if (!ud_was) {
            menu_mode = !menu_mode;
            if (menu_mode) Serial.printf("[menu] L=stop  R=play  U/D=track  (key/inst exits)\n");
            else           Serial.printf("[menu] exit\n");
            Serial.flush();
            ud_was = true;
        }
        return;   // hold U+D: stay put, don't run voicings
    }
    ud_was = false;
    if (menu_mode) {
        // edge-triggered actions: one per press of L / R / U / D
        bool mL = lf, mR = rt, mU = up && !dn, mD = dn && !up;
        if (mL && !m_l_was) {
            ibxm_sequence_stop(g_player);
            cur_inst = 1;   // reset to the first instrument on stop
            Serial.printf("[menu] stopped\n"); Serial.flush();
        } else if (mR && !m_r_was) {
            ibxm_sequence_play(g_player);   // unmute + restart from top
            Serial.printf("[menu] playing original\n"); Serial.flush();
        } else if (mU && !m_u_was) {
            g_track = (g_track + 1) % 2;
            if (load_track()) { Serial.printf("[menu] track %d: %s\n", g_track+1, TRACK_NAMES[g_track]); Serial.flush(); }
        } else if (mD && !m_d_was) {
            g_track = (g_track + 1) % 2;
            if (load_track()) { Serial.printf("[menu] track %d: %s\n", g_track+1, TRACK_NAMES[g_track]); Serial.flush(); }
        }
        m_l_was=mL; m_r_was=mR; m_u_was=mU; m_d_was=mD;
        return;
    }
    if (chord_menu) {
        // edge-triggered: U/D = change root note, L = toggle single-note mode
        bool cU = up && !dn, cD = dn && !up, cL = lf && !rt;
        if (cU && !c_u_was) {
            key_root = (key_root + 1) % 12;
            Serial.printf("[cmenu] root=%s\n", NOTE_NAMES[key_root]); Serial.flush();
            for (int k=0;k<7;k++) if (key_held[k]) refresh_held();
        } else if (cD && !c_d_was) {
            key_root = (key_root + 11) % 12;
            Serial.printf("[cmenu] root=%s\n", NOTE_NAMES[key_root]); Serial.flush();
            for (int k=0;k<7;k++) if (key_held[k]) refresh_held();
        } else if (cL && !c_l_was) {
            single_note = !single_note;
            Serial.printf("[cmenu] single_note %s\n", single_note ? "on" : "off"); Serial.flush();
            for (int k=0;k<7;k++) if (key_held[k]) refresh_held();
        }
        c_u_was=cU; c_d_was=cD; c_l_was=cL;
        return;
    }

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

void setup() {
    Serial.begin(115200);
    for (int k = 0; k < 7; k++) { pinMode(KEY_PINS[k], INPUT_PULLUP); key_held[k] = false; }
    for (int d = 0; d < 4; d++) pinMode(DPAD_PINS[d], INPUT_PULLUP);
    pinMode(INST_PIN, INPUT_PULLUP);

    Serial.println("[ibmxchord] booting..."); Serial.flush();
    if (!psramFound()) Serial.println("WARN: no PSRAM");

    if (!init_i2s()) { Serial.println("i2s init failed"); while (1) delay(1000); }
    g_ring = ibxm_ring_create(RING_FRAMES, SAMPLE_RATE);
    if (!g_ring) { Serial.println("ring alloc failed"); while (1) delay(1000); }

    if (!load_module(TRACK_LABELS[g_track])) { Serial.println("module load failed"); while (1) delay(1000); }
    Serial.printf("[track %d] %s\n", g_track + 1, TRACK_NAMES[g_track]); Serial.flush();

    ibxm_sequence_stop(g_player);   // mute sequencer + hard-stop all channels at boot
    memset(chan_pool, 0, sizeof(chan_pool));
    memset(key_chans, -1, sizeof(key_chans));

    g_running = true;
    xTaskCreatePinnedToCore(render_task, "render", 4096, nullptr, 5, &g_render_task, 1);
    xTaskCreatePinnedToCore(i2s_feed_task, "i2s", 4096, nullptr, 5, &g_i2s_task, 1);

    Serial.printf("[ibmxchord ready] '%s' %d ch, inst=1/%d (of %d hdr), key=C oct=0\n",
                  g_player->module->name, g_player->module->num_channels,
                  g_player->module->num_playable, g_player->module->num_instruments);
    Serial.flush();
}

void loop() {
    scan_buttons();
    vTaskDelay(pdMS_TO_TICKS(5));
}
