/*
 * ibxm_ring.h - render/consume ring buffer for ibxm on ESP32.
 *
 * The ibxm decoder (replay_get_audio) only emits ONE tick at a time, and a tick
 * is a variable number of stereo frames (tick_len = (sample_rate*5)/(tempo*2),
 * so it changes whenever tempo changes). It also keeps a ~130-frame overlap
 * tail for the volume ramp / downsample, which must NOT be sliced through.
 *
 * This ring sits between the decoder and the I2S DMA: a producer task calls
 * ibxm_ring_push_tick() (which renders a whole tick and stores it contiguously,
 * converting to int16), and the DMA feed callback calls ibxm_ring_pull() to
 * grab a fixed number of stereo frames. The ring absorbs the variable tick
 * size and the overlap, so the DMA side always gets clean, fixed-size blocks.
 *
 * Single-producer (one render task) / single-consumer (one DMA callback) and
 * therefore lock-free via atomic indices. Fallback mutex is provided only for
 * the blocking wait, which on ESP32 uses a FreeRTOS counting semaphore.
 */
#ifndef _IBXM_RING_H
#define _IBXM_RING_H

#include <stdint.h>
#include "ibxm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ibxm_ring ibxm_ring_t;

/* Create a ring that can hold at least `capacity_frames` stereo (L/R) int16
 * frames. Actual capacity is rounded up to a power of two. The ring buffer is
 * kept in internal DRAM (small, low-latency) because the DMA/ISR consumer
 * touches it on the hot path. Returns NULL on allocation failure. */
ibxm_ring_t *ibxm_ring_create( int capacity_frames, int sample_rate );

/* Free a ring created with ibxm_ring_create(). */
void ibxm_ring_destroy( ibxm_ring_t *ring );

/* Producer: render one full tick from `replay` and push the resulting
 * (variable) number of stereo frames into the ring, converting to int16.
 * Returns the number of frames pushed (>0), or 0 if the ring was full (caller
 * should retry after the consumer drains), or <0 on error. */
int ibxm_ring_push_tick( struct replay *replay, ibxm_ring_t *ring );

/* Consumer: copy up to `frames` stereo int16 samples from the ring into `out`
 * (layout: out[2*i] = L, out[2*i+1] = R). Never splits a tick and never reads
 * past what the producer wrote. Returns the number of frames actually copied
 * (which may be less than `frames` if the ring is short on data). */
int ibxm_ring_pull( ibxm_ring_t *ring, int16_t *out, int frames );

/* Number of stereo frames currently available to pull. */
int ibxm_ring_available( ibxm_ring_t *ring );

/* Block the caller until at least `frames` are available or `wait_ms` elapse.
 * wait_ms <= 0 returns immediately. Returns available frames after the wait.
 * On a non-FreeRTOS host build this is a best-effort poll. */
int ibxm_ring_wait( ibxm_ring_t *ring, int frames, int wait_ms );

#ifdef __cplusplus
}
#endif

#endif /* _IBXM_RING_H */
