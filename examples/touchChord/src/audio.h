// Audio / instrument engine for Ibmxchord.
// Owns: the ibxm player + render/I2S pipeline, track loading (flash + SD),
// the chord / single-note / drum engines, song playback, and the menu value
// hooks that read and drive this state.
#ifndef HC_AUDIO_H
#define HC_AUDIO_H

#include <Arduino.h>
#include "ibxm.h"

// ---- shared state (defined in audio.cpp) ----
extern struct ibxm_player *g_player;      // the loaded module
extern int   g_track;                     // flash-partition track index
extern const char *TRACK_NAMES[2];
extern int8_t key_root;                   // 0..11 from C
extern int8_t octave;                     // -2..+2
extern int8_t cur_inst;                   // 1-based instrument the keys play
extern bool   single_note;                // true = one tone per key, false = chord
extern bool   drum_mode;                  // true = per-button instrument hits
extern bool   song_playing;               // original tracker song audible?
extern bool   key_held[7];
extern int    active_voicing;             // D-pad recolor (0 = base triad)
extern int    dpad_v;
extern const char *NOTE_NAMES[12];
extern const char *VOICE_NAMES[9];

// ---- pipeline / engine ----
bool init_i2s();
bool load_track();                        // (re)load the selected flash track
void reset_to_ibmxchord();                // silence all + clean chord state
void trigger_chord(int deg);              // key press (deg 0..6)
void release_chord(int deg);              // key release
void apply_voicing(int v);                // recolor held chord (0..8)
void refresh_held();                      // re-strike after root/octave/inst change
void song_play();
void song_stop();

// ---- instrument/track introspection (used by menus + UI) ----
int  inst_count();
int  inst_number(int idx);                // playable index -> 1-based instrument number
int  track_count();
void track_label(int i, char *buf, int bl);
void track_info(int i, char *buf, int bl);   // right-column: "D" or size
void track_select(int i);
bool track_back(void);              // L: up one folder in LOAD TRACK

// ---- SD card ----
void mount_sd();
void scan_sd();

// ---- menu value hooks (referenced from the menu tables in main.cpp) ----
int  get_root(int idx);
void adj_root(int idx, int d);
int  get_oct(int idx);
void adj_oct(int idx, int d);
int  get_sn(int idx);
void adj_sn(int idx, int d);
int  get_drummode(int idx);
void adj_drummode(int idx, int d);
int  get_druminst(int idx);
void adj_druminst(int idx, int d);
int  get_drumtune(int idx);
void adj_drumtune(int idx, int d);
void fmt_druminst(int idx, int v, char *buf, int bl);
void fmt_drumtune(int idx, int v, char *buf, int bl);

#endif  // HC_AUDIO_H
