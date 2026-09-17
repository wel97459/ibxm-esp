#ifndef ESP_TTF_BIGFONT
#define  ESP_TTF_BIGFONT
    #ifdef __cplusplus
    extern "C" {
    #endif
    void tft_bigfont_putc(const uint8_t x, const uint8_t y, const uint8_t pal, const char c);
    void tft_bigfont_puts(const uint8_t x, const uint8_t y, const uint8_t pal, char *str);
    void tft_bigfont_printf(const uint8_t x, const uint8_t y, const uint8_t pal, const char *format, ...);

    void tft_lilfont_putc(const uint8_t x, const uint8_t y, const uint8_t pal, const char c);
    void tft_lilfont_puts(const uint8_t x, const uint8_t y, const uint8_t pal, char *str);
    void tft_lilfont_printf(const uint8_t x, const uint8_t y, const uint8_t pal, const char *format, ...);

    void tft_bigfont_win_putc(const uint8_t x, const uint8_t y, const uint8_t pal, const char c);
    void tft_bigfont_win_puts(const uint8_t x, const uint8_t y, const uint8_t pal, char *str);
    void tft_bigfont_win_printf(const uint8_t x, const uint8_t y, const uint8_t pal, const char *format, ...);
    #ifdef __cplusplus
    }
    #endif
#endif
