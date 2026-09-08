
/* ibxm/ac mod/xm/s3m replay (c)mumart@gmail.com */
#ifndef _IBXM_H
#define _IBXM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char *IBXM_VERSION;

/* Allocation strategy (ESP32):
   - Control structures (module, patterns, replay, channels, ramp_buf) are kept
     in internal DRAM because they are touched on every tick in the hot path.
     Routing them through PSRAM (as the old "#define calloc" did) causes cache
     misses and hurts throughput.
   - Large PCM sample data is the real memory hog and is allocated in PSRAM via
     ibxm_calloc_psram() (see ibxm.c). Fallback to internal DRAM if no PSRAM.
   We deliberately do NOT redefine the libc calloc() globally. */

struct data {
	char *buffer;
	int length;
};

struct sample {
	char name[ 32 ];
	int loop_start, loop_length;
	short volume, panning, rel_note, fine_tune, *data;
	/* Number of mono samples in data[] (0 if no wave decoded). Used to decide
	   whether an instrument is actually playable, independent of the module's
	   (often padded) instrument count. */
	int data_length;
	/* Streaming (ESP32 DRAM-constrained) source descriptor. When module->stream
	   is set, sample->data is decoded lazily from module->src on first use and
	   evicted under a DRAM budget; the source params are stored here instead. */
	int src_offset, src_length, sixteen_bit, is_signed, lru, in_use;
};

struct envelope {
	char enabled, sustain, looped, num_points;
	short sustain_tick, loop_start_tick, loop_end_tick;
	short points_tick[ 16 ], points_ampl[ 16 ];
};

struct instrument {
	int num_samples, vol_fadeout;
	char name[ 32 ], key_to_sample[ 97 ];
	char vib_type, vib_sweep, vib_depth, vib_rate;
	struct envelope vol_env, pan_env;
	struct sample *samples;
};

struct pattern {
	int num_channels, num_rows;
	char *data;
	/* Streaming: when module->stream is set, pattern->data is unpacked lazily
	   from module->src at packed_offset (absolute source byte offset) and
	   evicted under an LRU of at most IBXM_MAX_RESIDENT_PATTERNS. */
	int packed_offset, lru, unpacked;
};

struct module {
	char name[ 32 ];
	int num_channels, num_instruments;
	int num_playable;	/* instruments that actually carry a sample (S3M header
				   count is often padded; use this for cycling) */
	int num_patterns, sequence_len, restart_pos;
	int default_gvol, default_speed, default_tempo, c2_rate, gain;
	int linear_periods, fast_vol_slides;
	unsigned char *default_panning, *sequence;
	struct pattern *patterns;
	struct instrument *instruments;
	int seq_muted;  /* HiChord mode: sequencer injects no notes (pattern_get_note returns silence). */
	/* Streaming source: for the openArray (flash-mmap) path this points at the
	   durable, already-in-memory module data so samples/patterns can be
	   decoded on demand. */
	struct data *src;
	int stream;
	/* S3M channel map (derived once at load) so patterns can be unpacked on
	   demand with the identical mapping used during eager load. */
	int channel_map[ 32 ];
};

struct note {
	unsigned char key, instrument, volume, effect, param;
};

struct channel {
	struct replay *replay;
	struct instrument *instrument;
	struct sample *sample;
	struct note note;
	int id, key_on, random_seed, pl_row;
	int sample_off, sample_idx, sample_fra, freq, ampl, pann;
	int volume, panning, fadeout_vol, vol_env_tick, pan_env_tick;
	int release_fade;	/* HiChord: per-channel volume fade rate applied after
				   key-off when the instrument has no volume envelope,
				   so notes get a release tail instead of cutting dead.
				   0 = use the instrument's own vol_fadeout (default). */
	int period, porta_period, retrig_count, fx_count, av_count;
	int porta_up_param, porta_down_param, tone_porta_param, offset_param;
	int fine_porta_up_param, fine_porta_down_param, xfine_porta_param;
	int arpeggio_param, vol_slide_param, gvol_slide_param, pan_slide_param;
	int fine_vslide_up_param, fine_vslide_down_param;
	int retrig_volume, retrig_ticks, tremor_on_ticks, tremor_off_ticks;
	int vibrato_type, vibrato_phase, vibrato_speed, vibrato_depth;
	int tremolo_type, tremolo_phase, tremolo_speed, tremolo_depth;
	int tremolo_add, vibrato_add, arpeggio_add;
};

struct replay {
	int sample_rate, interpolation, global_vol;
	int seq_pos, break_pos, row, next_row, tick;
	int speed, tempo, pl_count, pl_chan;
	int *ramp_buf;
	char **play_count;
	struct channel *channels;
	struct module *module;
};

struct ibxm_player {
	struct replay *replay;
	struct module *module;
	int tick_len;
	int duration;
};

/* Allocate and initialize a module from the specified data, returns NULL on error.
   Message should point to a 64-character buffer to receive error messages. */
struct module* module_load( struct data *data);
/* stream != 0 enables on-demand decoding of PCM samples and pattern tables
   from the durable module source (used by the openArray / flash-mmap path on
   DRAM-constrained ESP32 where PSRAM is unavailable). */
struct module* module_load_ex( struct data *data, int stream );
/* Deallocate the specified module. */
void dispose_module( struct module *module );
/* Allocate and initialize a replay with the specified module and sampling rate. */
struct replay* new_replay( struct module *module, int sample_rate, int interpolation );
/* Deallocate the specified replay. */
void dispose_replay( struct replay *replay );
/* Returns the song duration in samples at the current sampling rate. */
int replay_calculate_duration( struct replay *replay );
/* Seek to approximately the specified sample position.
   The actual sample position reached is returned. */
int replay_seek( struct replay *replay, int sample_pos );
/* Set the pattern in the sequence to play. The tempo is reset to the default. */
void replay_set_sequence_pos( struct replay *replay, int pos );
/* Generates audio and returns the number of stereo samples written into mix_buf. */
int replay_get_audio( struct replay *replay, int *mix_buf, int tick_len );
/* Returns the length of the output buffer required by replay_get_audio(). */
int calculate_mix_buf_len( int sample_rate );

int replay_calculate_tick_len( struct replay *replay);

struct ibxm_player * play_module(struct data *d, int sample_rate, int interpolation);
struct ibxm_player * play_module_stream(struct data *d, int sample_rate, int interpolation, int stream);
struct ibxm_player * openFile(char *filename, int sample_rate, int interpolation);
struct ibxm_player * openArray(const uint8_t *dataIn, uint32_t len, int sample_rate, int interpolation);

/* ---- Direct instrument trigger API (HiChord mode) ----
   Bypass the file's pattern sequencer: ibxm_sequence_mute() blanks the
   sequencer (patterns still tick for envelopes/effects but inject no notes),
   then note_on()/note_off() inject notes straight into replay channels.
   key = 1..96 (semitones; 60 = middle C), instrument = 1-based index into
   the module's instrument list. The full voice engine (loops, envelopes,
   vibrato, volume ramps) runs exactly as in tracker playback. */
void ibxm_sequence_mute( struct ibxm_player *player );
void ibxm_sequence_unmute( struct ibxm_player *player );
void ibxm_restart( struct ibxm_player *player );
/* Return the next 1-based instrument index >= 1 that actually carries a decoded
   wave (non-null, non-zero-length sample), starting the search just after
   `from` and wrapping around. Returns `from` if no other playable instrument
   exists, so a caller can never spin forever. Robust against padded headers. */
int ibxm_next_instrument( struct ibxm_player *player, int from );
/* Copy the name of instrument `ins` (1-based) into `buf` (size `len`, including
   NUL). Returns buf. Empty string if ins is out of range. */
char *ibxm_instrument_name( struct ibxm_player *player, int ins, char *buf, int len );
void ibxm_note_on( struct ibxm_player *player, int channel, int key, int instrument, int volume );
void ibxm_note_off( struct ibxm_player *player, int channel );

#ifdef __cplusplus
}
#endif

#endif