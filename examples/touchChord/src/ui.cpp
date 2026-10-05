// Ibmxchord UI: ST7789 display init, status page, menu renderer, song
// channel view. Everything that touches the screen lives here.

#include <Arduino.h>
#include "ibxm.h"
#include "esp_tft.h"       // reused ST7789 240x240 4-wire SPI driver (ESP_TFT)
#include "esp_tft_tile.h"  // tile + 4-color palette layer
#include "esp_tft_bigfont.h" // 8x8 / big font on top of the tile layer
#include "bigfont_data.c"  // tiles_bigfont[] generated from bigfont.chr
extern "C" const uint8_t gfx_tiles[];   // gui.chr tiles (24 bytes each), from gui_tiles_data.c

#include "audio.h"
#include "menu.h"
#include "ui.h"

bool display_ready = false;

// ---- static background art ----
static void draw_screen_border() {
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

static void drawGrid() {
    for (size_t y = 0; y < 30; y++) {
        for (size_t x = 0; x < 30; x++) {
            tft_tile_putBackgoundGFX(x, y, 3, &gfx_tiles[0x13*0x18]);
        }
    }
}

void display_init() {
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
    // palette 3 = grid palette: the grid tile's line pixels are color index 2
    // (not 3), so dark gray goes in slot 2
    tft_tile_putPalette(3, TFT_BLACK, TFT_BLACK, tft_color565(0x0a, 0x0a, 0x0a), TFT_RED);

    draw_screen_border();
    drawGrid();

    Serial.println("[display] ESP_TFT ready");
}

// "LOADING" dialog. NOT drawn here — track loading runs in btn_task and
// drawing from two tasks races in the tile buffers/SPI driver. ui_loading()
// only records the name; draw_ui() renders the dialog on its next pass and
// keeps it up until ui_loading_done() clears the flag after the load.
static char         g_loading_name[24];
static volatile bool g_loading = false;

void ui_loading(const char *name) {
    strncpy(g_loading_name, name ? name : "", sizeof(g_loading_name) - 1);
    g_loading_name[sizeof(g_loading_name) - 1] = 0;
    g_loading = true;
}
void ui_loading_done(void) { g_loading = false; }

// Song view: which instrument each channel is currently sounding.
static void draw_song_view() {
    if (!display_ready || !g_player) return;
    tft_tile_clear();
    tft_lilfont_printf(1, 1, TXT_PAL(0), "Song  %s", TRACK_NAMES[g_track]);
    tft_lilfont_printf(1, 4, TXT_PAL(0), song_playing ? "PLAYING" : "stopped");

    int row = 0;
    int nch = g_player->module->num_channels;
    for (int ch = 0; ch < nch && row < 24; ch++) {
        int ins = ibxm_channel_instrument(g_player, ch);
        if (!ins) continue;                       // idle channel: skip
        char nm[22]; nm[0] = 0;
        ibxm_instrument_name(g_player, ins, nm, sizeof(nm));
        tft_lilfont_printf(1, 7 + row * 2, TXT_PAL(0), "c%02d %s", ch + 1, nm);
        row++;
    }
    if (row == 0)
        tft_lilfont_printf(1, 7, TXT_PAL(0), "(no channels active)");

    for (uint8_t y = 0; y < 30; y++) tft_tile_render(y, 0, 0);
    tft_tile_sendLine(29*8, 0);
}

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
                if (strlen(lbl) > 22) lbl[22] = 0;    // leave room for the right column
                tft_lilfont_printf(1, y, TXT_PAL(rp), "%s%s", cursor, lbl);
                char info[8]; track_info(idx, info, sizeof(info));
                if (info[0]) tft_lilfont_printf(28 - strlen(info), y, TXT_PAL(rp), "%s", info);
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
void draw_ui() {
    if (!display_ready) return;
    if (millis() - ui_t0 < 120) return;   // ~8 fps refresh
    ui_t0 = millis();

    if (g_loading) {                        // loading dialog over everything
        tft_tile_clear();
        tft_lilfont_printf(1, 11, TXT_PAL(1), "LOADING...");
        tft_lilfont_printf(1, 14, TXT_PAL(0), "%s", g_loading_name);
        for (uint8_t y = 0; y < 30; y++) tft_tile_render(y, 0, 0);
        tft_tile_sendLine(29*8, 0);
        return;
    }
    if (menu_open) { draw_menu(); return; }   // menu replaces the status page
    if (song_playing) { draw_song_view(); return; }   // channel view while the song plays

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
void ui_task(void*) {
    for (;;) {
        draw_ui();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
