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
//
// Split: menu tables + input scanning + setup live here; the instrument
// engine lives in audio.cpp and all display drawing in ui.cpp.

#include <Arduino.h>
#include "ibxm.h"
#include "audio.h"
#include "menu.h"
#include "ui.h"

// ---- buttons: 7 chord keys + 4 dpad (to GND, internal pullups) ----
#ifndef HC_KEY_PINS
#define HC_KEY_PINS {1, 2, 3, 4, 8, 10, 13}
#endif
static const uint8_t KEY_PINS[7]  = HC_KEY_PINS;
#ifndef HC_DPAD_PINS
#define HC_DPAD_PINS {34, 35, 36, 37}
#endif
static const uint8_t DPAD_PINS[4] = HC_DPAD_PINS; // L R U D — GPIO34-37 are input-only on S2, perfect for buttons
#ifndef HC_INST_PIN
#define HC_INST_PIN 18
#endif
static const uint8_t INST_PIN = HC_INST_PIN;      // menu toggle (free GPIO, not a strapping pin like GPIO0)

// ================= reusable menu object =================
bool        menu_open = false;
int         menu_sel  = 0;
const Menu *menu_cur  = nullptr;
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

extern const Menu MENU_DRUMS;   // defined below (referenced by the Sound submenu)
extern const Menu MENU_SONG;    // defined below (referenced by the root menu)
extern const Menu MENU_FX;      // defined below (referenced by the root menu)

static const char *SN_VALS[2]   = { "off", "on" };
static const char *DRUM_VALS[2] = { "off", "on" };
static const MenuItem ITEMS_SOUND[] = {
    { "Single Note", MI_VALUE, get_sn,   SN_VALS,   2, adj_sn,   nullptr, nullptr, 0, nullptr },
    { "Drum Mode",   MI_VALUE, get_drummode, DRUM_VALS, 2, adj_drummode, nullptr, nullptr, 0, nullptr },
    { "Drums",       MI_SUBMENU, nullptr, nullptr, 0, nullptr, &MENU_DRUMS, nullptr, 0, nullptr },
    { "Back",        MI_ACTION, nullptr, nullptr, 0, nullptr, nullptr, [](){ menu_back(); }, 0, nullptr },
};
static const Menu MENU_SOUND = { "SOUND", ITEMS_SOUND, 4 };

const Menu MENU_INST  = { "INSTRUMENT", nullptr, 0 };  // dynamic (MI_INSTLIST)
const Menu MENU_TRACK = { "LOAD TRACK", nullptr, 0 };  // dynamic (MI_TRACKLIST)

static const MenuItem ITEMS_ROOT[] = {
    { "Instrument", MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_INST, nullptr },
    { "Sound",      MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_SOUND, nullptr },
    { "Root",       MI_VALUE,    get_root, NOTE_NAMES, 12, adj_root, nullptr, nullptr },
    { "Octave",     MI_VALUE,    get_oct,  nullptr, 0, adj_oct, nullptr, nullptr },
    { "Load Track", MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_TRACK, nullptr },
    { "Song",       MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_SONG, nullptr },
    { "FX",         MI_SUBMENU,  nullptr, nullptr, 0, nullptr, &MENU_FX, nullptr },
    { "Close",      MI_ACTION,   nullptr, nullptr, 0, nullptr, nullptr, [](){ menu_close(); } },
};
static const Menu MENU_ROOT = { "IBMXCHORD MENU", ITEMS_ROOT, 8 };

static const MenuItem ITEMS_FX[] = {
    { "Reverb",     MI_VALUE, get_fxrev,  nullptr, 0, adj_fxrev,  nullptr, nullptr, 0, nullptr },
    { "Delay",      MI_VALUE, get_fxdly,  nullptr, 0, adj_fxdly,  nullptr, nullptr, 0, nullptr },
    { "Delay time", MI_VALUE, get_fxdlyms, nullptr, 0, adj_fxdlyms, nullptr, nullptr, 0, nullptr },
    { "Volume",     MI_VALUE, get_fxvol,   nullptr, 0, adj_fxvol,   nullptr, nullptr, 0, nullptr },
    { "Back",       MI_ACTION, nullptr, nullptr, 0, nullptr, nullptr, [](){ menu_back(); }, 0, nullptr },
};
const Menu MENU_FX = { "FX", ITEMS_FX, 5 };

static const MenuItem ITEMS_SONG[] = {
    { "Play original", MI_ACTION, nullptr, nullptr, 0, nullptr, nullptr, [](){ song_play(); }, 0, nullptr },
    { "Stop",          MI_ACTION, nullptr, nullptr, 0, nullptr, nullptr, [](){ song_stop(); }, 0, nullptr },
    { "Back",          MI_ACTION, nullptr, nullptr, 0, nullptr, nullptr, [](){ menu_back(); }, 0, nullptr },
};
const Menu MENU_SONG = { "SONG", ITEMS_SONG, 3 };

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
            if (lf && !l_was) {
                if (menu_cur == &MENU_TRACK && track_back()) { menu_sel = 0; }  // up one folder
                else menu_back();            // L = back in these list submenus
            }
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

    Serial.println("[ibmxchord] booting..."); Serial.flush();
    if (!psramFound()) Serial.println("WARN: no PSRAM");

    display_init();
    mount_sd();
    scan_sd();

    if (!init_i2s()) { Serial.println("i2s init failed"); while (1) delay(1000); }
    if (!load_track()) { Serial.println("module load failed"); while (1) delay(1000); }
    Serial.printf("[track %d] %s\n", g_track + 1, TRACK_NAMES[g_track]); Serial.flush();

    xTaskCreatePinnedToCore(ui_task, "ui", 8192, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(btn_task, "btn", 4096, nullptr, 3, nullptr, 1);

    Serial.printf("[ibmxchord ready] '%s' %d ch, inst=1/%d (of %d hdr), key=C oct=0\n",
                  g_player->module->name, g_player->module->num_channels,
                  g_player->module->num_playable, g_player->module->num_instruments);
    Serial.flush();
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }   // unused: UI runs in ui_task (core 0)
