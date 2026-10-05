// Final-output FX for Ibmxchord: feedback delay + simple reverb, applied
// to the stereo mix in the I2S feed task (after ibxm_ring_pull, before
// i2s_write). All buffers live in internal RAM; amounts 0..9, 0 = off.
#ifndef HC_FX_H
#define HC_FX_H

#include <stdint.h>
#include <stddef.h>

void fx_init(int sample_rate);
void fx_process(int16_t *stereo, int frames);   // frames of L,R int16 pairs
void fx_reset();                                // clear tails (on track load/stop)

// ---- menu hooks ----
extern int fx_delay_amt;    // 0..9 (0 = off)
extern int fx_delay_ms;     // delay line length, 40..400 ms
extern int fx_reverb_amt;   // 0..9 (0 = off)
extern int fx_volume;       // 0..9 output gain steps: 0.25x..2.0x, 4 = unity

#endif  // HC_FX_H
