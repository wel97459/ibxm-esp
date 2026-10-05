// UI for Ibmxchord: ST7789 240x240 display (ESP_TFT + tile + bigfont),
// status page, menu renderer, song channel view. All drawing lives here;
// the UI task is pinned to core 0 so it never competes with audio (core 1).
#ifndef HC_UI_H
#define HC_UI_H

#include <Arduino.h>

extern bool display_ready;   // display came up at boot (mount_sd waits for it)

void display_init();         // init tft + tile engine + palettes + bg
void draw_ui();              // one throttled frame (menu / song view / status)
void ui_task(void *arg);     // endless draw loop (spawned by setup)

// Transparent-text palette helper: TRANSPARENT_TILE (0x20) makes glyph
// background pixels (b==0) keep the layer below.
#define TXT_PAL(p) ((p) | TRANSPARENT_TILE)

#endif  // HC_UI_H
