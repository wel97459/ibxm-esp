/*
 * example/main.cpp - load an XM module from a raw flash partition and play it
 *                    through I2S using the ibxm decoder + ibxm_ring ring buffer.
 *
 * Wiring (external DAC / class-D amp such as MAX98357 or PT8211):
 *   ESP32 BCK  -> DAC BCK
 *   ESP32 WS   -> DAC WS/LRCLK
 *   ESP32 DATA -> DAC DIN
 *   (no MCLK required for these DACs)
 *
 * Flash the module into the 'xm_music' partition, e.g. with esptool:
 *   esptool.py --port /dev/ttyUSB0 write_flash 0x200000 your_song.xm
 * (offset 0x200000 = factory 0x10000 + 0x1F0000; confirm with 'partitions.csv'
 *  by summing the sizes of the partitions that precede xm_music).
 *
 * Build (PlatformIO): this file lives in examples/player/src/main.cpp. The
 * example's platformio.ini builds it and pulls the ibxm decoder library from
 * the parent repo via lib_deps = file://../...
 */
#include <Arduino.h>
#include "driver/i2s.h"
#include "esp_partition.h"

extern "C" {
#include "ibxm.h"
#include "ibxm_ring.h"
}

/* ---- I2S pin assignment (WROVER-KIT free GPIOs; avoid flash 6-11, PSRAM 16/17).
   Wire an external DAC/amp (MAX98357 or PT8211):
     ESP32 BCK  -> DAC BCK
     ESP32 WS   -> DAC WS/LRCLK
     ESP32 DATA -> DAC DIN
   (No MCLK required for those DACs.) ---- */
#ifndef I2S_BCK_PIN
#define I2S_BCK_PIN   26
#endif
#ifndef I2S_WS_PIN
#define I2S_WS_PIN    25
#endif
#ifndef I2S_DATA_PIN
#define I2S_DATA_PIN  21
#endif
#ifndef I2S_PORT
#define I2S_PORT      I2S_NUM_0
#endif

/* ---- Audio config ---- */
#define SAMPLE_RATE        44100
#define INTERPOLATION      1           /* 1 = linear, 0 = nearest (faster) */
#define RING_FRAMES        2048        /* ~46 ms of slack; ring is in DRAM */
#define I2S_DMA_BUF_COUNT  4
#define I2S_DMA_BUF_LEN    128         /* frames per DMA buffer */
#define I2S_WRITE_MS       20          /* ticks_to_wait for i2s_write */

/* ---- Partition that holds the module ---- */
#define XM_PARTITION_LABEL "xm_music"

static ibxm_ring_t      *g_ring      = nullptr;
static struct ibxm_player *g_player   = nullptr;
static volatile bool     g_playing   = false;

/* Render task: pull ticks from the decoder into the ring. */
static void render_task(void *arg) {
    (void)arg;
    struct replay *replay = g_player->replay;
    while (g_playing) {
        /* push_tick renders one whole tick; returns 0 if the ring is full,
         * in which case we yield and let the I2S task drain it. */
        int n = ibxm_ring_push_tick(replay, g_ring);
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (n < 0) {
            log_e("replay ended or error");
            g_playing = false;
            break;
        }
    }
    vTaskDelete(nullptr);
}

/* I2S feed task: pull fixed-size frame blocks from the ring and write them. */
static void i2s_feed_task(void *arg) {
    (void)arg;
    const int pull_frames = I2S_DMA_BUF_LEN;
    int16_t *out = (int16_t *)malloc(pull_frames * 2 * sizeof(int16_t));
    if (!out) {
        log_e("out buf alloc failed");
        g_playing = false;
        vTaskDelete(nullptr);
    }
    while (g_playing) {
        int got = ibxm_ring_pull(g_ring, out, pull_frames);
        if (got == 0) {
            /* Ring is momentarily empty; idle a touch to avoid busy-spin. */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        size_t written = 0;
        /* i2s_write wants bytes; stereo int16 = 4 bytes/frame. */
        i2s_write(I2S_PORT, out, (size_t)got * 2 * sizeof(int16_t),
                  &written, pdMS_TO_TICKS(I2S_WRITE_MS));
    }
    free(out);
    vTaskDelete(nullptr);
}

static bool init_i2s() {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = I2S_DMA_BUF_COUNT;
    cfg.dma_buf_len          = I2S_DMA_BUF_LEN;
    cfg.tx_desc_auto_clear   = true;   /* silence on underrun instead of repeats */
    cfg.use_apll             = false;

    i2s_pin_config_t pins = {};
    pins.bck_io_num     = I2S_BCK_PIN;
    pins.ws_io_num      = I2S_WS_PIN;
    pins.data_out_num   = I2S_DATA_PIN;
    pins.data_in_num    = I2S_PIN_NO_CHANGE;
    pins.mck_io_num     = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) {
        log_e("i2s_driver_install failed");
        return false;
    }
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        log_e("i2s_set_pin failed");
        return false;
    }
    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

static bool load_module_from_partition() {
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 (esp_partition_subtype_t)0x40,
                                 XM_PARTITION_LABEL);
    if (!part) {
        log_e("partition '%s' not found - did you flash an XM there?",
              XM_PARTITION_LABEL);
        return false;
    }
    log_i("xm partition: offset=0x%08x size=0x%08x", part->address, part->size);

    /* Memory-map the partition read-only (flash -> DROM, no RAM copy). */
    const void *mapped = nullptr;
    spi_flash_mmap_handle_t handle = 0;
    esp_err_t e = esp_partition_mmap(part, 0, part->size,
                                     SPI_FLASH_MMAP_DATA, &mapped, &handle);
    if (e != ESP_OK) {
        log_e("esp_partition_mmap failed: %d", e);
        return false;
    }
    {
        const uint8_t *m = (const uint8_t *)mapped;
        log_e("DBG mmap ptr=%p size=0x%x [0..3]=%02x%02x%02x%02x [44..47]=%02x%02x%02x%02x",
              mapped, (unsigned)part->size, m[0], m[1], m[2], m[3],
              m[44], m[45], m[46], m[47]);
    }

    g_player = openArray((const uint8_t *)mapped, part->size,
                         SAMPLE_RATE, INTERPOLATION);
    if (!g_player) {
        log_e("ibxm failed to parse module (not an XM/MOD/S3M, or corrupt)");
        spi_flash_munmap(handle);
        return false;
    }
    log_i("loaded: duration=%d s, tick_len=%d, channels handled internally",
          g_player->duration / 1000, g_player->tick_len);
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(200);
    bool pf = psramFound();
    size_t free_spi = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    log_e("PSRAM found=%d size=%d free_spi=%u free_dram=%u", pf, ESP.getPsramSize(), free_spi, free_dram);
    if (!pf) {
        // Force init and report whether the SDK finds/initializes PSRAM at all.
        bool init_ok = psramInit();
        log_e("psramInit() returned %d; after-init found=%d free_spi=%u",
              init_ok, psramFound(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    if (!init_i2s()) {
        return;
    }
    log_e("after i2s_init free_dram=%u", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    /* Allocate the ring BEFORE the module, while the heap is still unfragmented
       (the module's many small allocations fragment DRAM; the ring needs large
       contiguous blocks). */
    g_ring = ibxm_ring_create(RING_FRAMES, SAMPLE_RATE);
    if (!g_ring) {
        log_e("ring alloc failed (free_dram=%u largest=%u)",
              heap_caps_get_free_size(MALLOC_CAP_8BIT),
              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        i2s_driver_uninstall(I2S_PORT);
        return;
    }

    if (!load_module_from_partition()) {
        ibxm_ring_destroy(g_ring);
        g_ring = NULL;
        i2s_driver_uninstall(I2S_PORT);
        return;
    }

    g_playing = true;
    xTaskCreatePinnedToCore(render_task, "ibxm_render", 4096,
                            nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(i2s_feed_task, "i2s_feed", 4096,
                            nullptr, 5, nullptr, 1);
    log_i("playing...");
}

void loop() {
    /* Nothing to do; everything runs in the two tasks. */
    delay(1000);
}
