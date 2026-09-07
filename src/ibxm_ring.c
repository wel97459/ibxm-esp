/*
 * ibxm_ring.c - render/consume ring buffer for ibxm on ESP32.
 * See ibxm_ring.h for the design rationale.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

#include "ibxm.h"
#include "ibxm_ring.h"

#ifndef IBXM_RING_TAG
#define IBXM_RING_TAG "IBXM_RING"
#endif

/* FreeRTOS is only present on the ESP target. Guard the blocking-wait path so
 * this file also compiles and links in a plain host test. */
#ifdef CONFIG_FREERTOS_UNICORE
#define IBXM_RING_FREERTOS 1
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

struct ibxm_ring {
	int16_t *buf;        /* capacity * 2 int16 = stereo frames */
	uint32_t cap;        /* power of two */
	uint32_t mask;       /* cap - 1 */
	int sample_rate;
	int max_tick_frames; /* largest tick_len this rate can produce */
	int *mix;            /* scratch int buffer for replay_get_audio */
	int mix_ints;        /* length of mix in ints */
	_Atomic uint32_t w;  /* total frames written (producer) */
	_Atomic uint32_t r;  /* total frames read (consumer) */
#ifdef IBXM_RING_FREERTOS
	SemaphoreHandle_t sem; /* signaled once per push_tick, for blocking wait */
#endif
};

/* Round up to the next power of two (safe for values < 2^31). */
static uint32_t round_pow2( uint32_t v ) {
	v--;
	v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
	return v + 1;
}

/* Largest possible tick (frames) for a sample rate: tempo is clamped >= 32 in
 * the decoder, so the max chunk is at tempo 32. */
static int max_tick_frames_for( int sample_rate ) {
	/* calculate_tick_len(tempo, sr) = (sr*5)/(tempo*2); tempo min 32. */
	return ( sample_rate * 5 ) / ( 32 * 2 );
}

ibxm_ring_t *ibxm_ring_create( int capacity_frames, int sample_rate ) {
	ibxm_ring_t *ring;
	uint32_t cap = round_pow2( (uint32_t) capacity_frames );
	int max_tick = max_tick_frames_for( sample_rate );

	if( cap < (uint32_t)( max_tick + 130 ) ) {
		cap = round_pow2( (uint32_t)( max_tick + 130 ) );
	}

	ring = calloc( 1, sizeof( ibxm_ring_t ) );
	if( !ring ) {
		return NULL;
	}
	ring->buf = calloc( cap * 2, sizeof( int16_t ) );
	if( !ring->buf ) {
		free( ring );
		return NULL;
	}
	/* scratch buffer must cover replay_get_audio's worst case:
	 * it memset()s (tick_len+65)*4 ints and reads up to idx+5, so
	 * (max_tick+65)*4 ints is sufficient with margin. */
	ring->mix_ints = ( max_tick + 65 ) * 4;
	ring->mix = calloc( ring->mix_ints, sizeof( int ) );
	if( !ring->mix ) {
		free( ring->buf );
		free( ring );
		return NULL;
	}
	ring->cap = cap;
	ring->mask = cap - 1;
	ring->sample_rate = sample_rate;
	ring->max_tick_frames = max_tick;
	atomic_store_explicit( &ring->w, 0, memory_order_relaxed );
	atomic_store_explicit( &ring->r, 0, memory_order_relaxed );

#ifdef IBXM_RING_FREERTOS
	ring->sem = xSemaphoreCreateBinary();
#endif
	return ring;
}

void ibxm_ring_destroy( ibxm_ring_t *ring ) {
	if( !ring ) {
		return;
	}
#ifdef IBXM_RING_FREERTOS
	if( ring->sem ) {
		vSemaphoreDelete( ring->sem );
	}
#endif
	free( ring->mix );
	free( ring->buf );
	free( ring );
}

static inline uint32_t ring_w( const ibxm_ring_t *ring ) {
	return atomic_load_explicit( &ring->w, memory_order_acquire );
}
static inline uint32_t ring_r( const ibxm_ring_t *ring ) {
	return atomic_load_explicit( &ring->r, memory_order_acquire );
}

int ibxm_ring_available( ibxm_ring_t *ring ) {
	uint32_t w = ring_w( ring );
	uint32_t r = ring_r( ring );
	return (int)( w - r );
}

static int ibxm_ring_free( ibxm_ring_t *ring ) {
	return (int)( ring->cap - ( ring_w( ring ) - ring_r( ring ) ) );
}

int ibxm_ring_push_tick( struct replay *replay, ibxm_ring_t *ring ) {
	int tick_len, i;
	int *mix;
	uint32_t w;

	if( !replay || !ring ) {
		return -1;
	}
	tick_len = replay_calculate_tick_len( replay );
	if( tick_len <= 0 ) {
		return -1;
	}

	/* Refuse to render unless the whole tick fits. Otherwise we would call
	 * replay_get_audio (which advances the decoder) and then drop frames,
	 * permanently desyncing the music. The producer must retry after the
	 * consumer drains. */
	if( ibxm_ring_free( ring ) < tick_len ) {
		return 0;
	}

	mix = ring->mix;
	/* Render one whole tick. replay_get_audio clears and fills mix and
	 * advances the song by exactly one tick. */
	replay_get_audio( replay, mix, tick_len );

	/* Convert the produced tick_len stereo frames (2 ints each) to int16 and
	 * write them by absolute index masked into the ring. This handles the
	 * physical wrap for us, so the tick's volume-ramp overlap tail is stored
	 * contiguously in logical order and never split across a DMA boundary. */
	w = ring_w( ring );
	for( i = 0; i < tick_len; i++ ) {
		int l = mix[ i * 2 ], r = mix[ i * 2 + 1 ];
		uint32_t idx = ( w + (uint32_t)i ) & ring->mask;
		if( l > 32767 ) l = 32767; else if( l < -32768 ) l = -32768;
		if( r > 32767 ) r = 32767; else if( r < -32768 ) r = -32768;
		ring->buf[ idx * 2 ]     = (int16_t) l;
		ring->buf[ idx * 2 + 1 ] = (int16_t) r;
	}
	atomic_store_explicit( &ring->w, w + (uint32_t)tick_len, memory_order_release );

#ifdef IBXM_RING_FREERTOS
	if( ring->sem ) {
		BaseType_t hp = pdFALSE;
		xSemaphoreGiveFromISR( ring->sem, &hp );
		(void)hp;
	}
#endif
	return tick_len;
}

int ibxm_ring_pull( ibxm_ring_t *ring, int16_t *out, int frames ) {
	uint32_t w, r, base;
	int avail, n;

	if( !ring || !out || frames <= 0 ) {
		return 0;
	}
	w = ring_w( ring );
	r = ring_r( ring );
	avail = (int)( w - r );
	if( avail <= 0 ) {
		return 0;
	}
	if( frames > avail ) {
		frames = avail;
	}
	base = r & ring->mask;
	n = ring->cap - base;          /* frames to the physical end */
	if( n > frames ) {
		n = frames;
	}
	/* First (possibly only) segment. */
	memcpy( out, &ring->buf[ base * 2 ], (size_t)n * 2 * sizeof( int16_t ) );
	/* Wrap segment. */
	if( n < frames ) {
		memcpy( &out[ n * 2 ], ring->buf,
			(size_t)( frames - n ) * 2 * sizeof( int16_t ) );
	}
	atomic_store_explicit( &ring->r, r + (uint32_t)frames, memory_order_release );
	return frames;
}

int ibxm_ring_wait( ibxm_ring_t *ring, int frames, int wait_ms ) {
	int avail = ibxm_ring_available( ring );
	if( avail >= frames ) {
		return avail;
	}
#ifdef IBXM_RING_FREERTOS
	if( ring->sem && wait_ms > 0 ) {
		TickType_t ticks = ( wait_ms / portTICK_PERIOD_MS ) + 1;
		if( xSemaphoreTake( ring->sem, ticks ) == pdTRUE ) {
			/* Producer signaled; re-check availability. */
			avail = ibxm_ring_available( ring );
		}
	}
#else
	(void)wait_ms; /* host build: caller polls available() */
#endif
	return avail;
}
