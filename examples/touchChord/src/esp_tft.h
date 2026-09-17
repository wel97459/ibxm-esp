#ifndef DEF_ESP_LCD
#define DEF_ESP_LCD
    #include <stddef.h>
    #include "driver/spi_master.h"

    #ifdef __cplusplus
    extern "C" {
    #endif

    typedef enum {
        LCD_TYPE_NONE,
        LCD_TYPE_ILI9163,
        LCD_TYPE_ST7789
    } type_lcd_t;

    // --- Lolin S2 Mini pin reassignment (touchChord) ---
    // Free GPIOs after I2S(5/6/7), keys(1/2/3/4/8/10/13), dpad(14/15/16/17), inst(18):
    // 33,34,35,36,37,38 are unused and all bidirectional on ESP32-S2 (only GPIO46
    // is input-only). Safe to use for the ST7789 SPI bus.
    #define PIN_NUM_MISO    34   // display is write-only; left unused
    #define PIN_NUM_MOSI    35   // SDA (data out)
    #define PIN_NUM_CLK     36   // SCL (clock)
    #define PIN_NUM_CS      37   // chip select (active low)

    #define PIN_NUM_DC      38   // data/command
    #define PIN_NUM_RST     33   // reset (active low)
    #define PIN_NUM_BCKL    34   // backlight (left unconnected here)

    #define PARALLEL_LINES  16

    #define TFT_NOP			0x00
    #define TFT_SWRESET		0x01
    #define TFT_RDDID		0x04
    #define TFT_RDDST		0x09

    #define TFT_RDDPM		0x0A      // Read display power mode
    #define TFT_RDD_MADCTL	0x0B      // Read display MADCTL
    #define TFT_RDD_COLMOD	0x0C      // Read display pixel format
    #define TFT_RDDIM		0x0D      // Read display image mode
    #define TFT_RDDSM		0x0E      // Read display signal mode
    #define TFT_RDDSR		0x0F      // Read display self-diagnostic result (TFTV)

    #define TFT_SLPIN		0x10
    #define TFT_SLPOUT		0x11
    #define TFT_PTLON		0x12
    #define TFT_NORON		0x13

    #define TFT_INVOFF		0x20
    #define TFT_INVON		0x21
    #define TFT_GAMSET		0x26      // Gamma set
    #define TFT_DISPOFF		0x28
    #define TFT_DISPON		0x29
    #define TFT_CASET		0x2A
    #define TFT_RASET		0x2B
    #define TFT_RAMWR		0x2C
    #define TFT_RGBSET		0x2D      // Color setting for 4096, 64K and 262K colors
    #define TFT_RAMRD		0x2E

    #define TFT_PTLAR		0x30
    #define TFT_VSCRDEF		0x33      // Vertical scrolling definition (TFTV)
    #define TFT_TEOFF		0x34      // Tearing effect line off
    #define TFT_TEON		0x35      // Tearing effect line on
    #define TFT_MADCTL		0x36      // Memory data access control
    #define TFT_IDMOFF		0x38      // Idle mode off
    #define TFT_IDMON		0x39      // Idle mode on
    #define TFT_RAMWRC		0x3C      // Memory write continue (TFTV)
    #define TFT_RAMRDC		0x3E      // Memory read continue (TFTV)
    #define TFT_COLMOD		0x3A

    #define TFT_RAMCTRL		0xB0      // RAM control
    #define TFT_RGBCTRL		0xB1      // RGB control
    #define TFT_PORCTRL		0xB2      // Porch control
    #define TFT_FRCTRL1		0xB3      // Frame rate control
    #define TFT_PARCTRL		0xB5      // Partial mode control
    #define TFT_GCTRL		0xB7      // Gate control
    #define TFT_GTADJ		0xB8      // Gate on timing adjustment
    #define TFT_DGMEN		0xBA      // Digital gamma enable
    #define TFT_VCOMS		0xBB      // VCOMS setting
    #define TFT_LCMCTRL		0xC0      // LCM control
    #define TFT_IDSET		0xC1      // ID setting
    #define TFT_VDVVRHEN	0xC2      // VDV and VRH command enable
    #define TFT_VRHS		0xC3      // VRH set
    #define TFT_VDVSET		0xC4      // VDV setting
    #define TFT_VCMOFSET	0xC5      // VCOMS offset set
    #define TFT_FRCTR2		0xC6      // FR Control 2
    #define TFT_CABCCTRL	0xC7      // CABC control
    #define TFT_REGSEL1		0xC8      // Register value section 1
    #define TFT_REGSEL2		0xCA      // Register value section 2
    #define TFT_PWMFRSEL	0xCC      // PWM frequency selection
    #define TFT_PWCTRL1		0xD0      // Power control 1
    #define TFT_VAPVANEN	0xD2      // Enable VAP/VAN signal output
    #define TFT_CMD2EN		0xDF      // Command 2 enable
    #define TFT_PVGAMCTRL	0xE0      // Positive voltage gamma control
    #define TFT_NVGAMCTRL	0xE1      // Negative voltage gamma control
    #define TFT_DGMLUTR		0xE2      // Digital gamma look-up table for red
    #define TFT_DGMLUTB		0xE3      // Digital gamma look-up table for blue
    #define TFT_GATECTRL	0xE4      // Gate control
    #define TFT_SPI2EN		0xE7      // SPI2 enable
    #define TFT_PWCTRL2		0xE8      // Power control 2
    #define TFT_EQCTRL		0xE9      // Equalize time control
    #define TFT_PROMCTRL	0xEC      // Program control
    #define TFT_PROMEN		0xFA      // Program mode enable
    #define TFT_NVMSET		0xFC      // NVM setting
    #define TFT_PROMACT		0xFE      // Program action

    #define TFT_BLACK       0x0000u      /*   0,   0,   0 */
    #define TFT_NAVY        0x000Fu      /*   0,   0, 128 */
    #define TFT_DARKGREEN   0x03E0u      /*   0, 128,   0 */
    #define TFT_DARKCYAN    0x03EFu      /*   0, 128, 128 */
    #define TFT_MAROON      0x7800u      /* 128,   0,   0 */
    #define TFT_PURPLE      0x780Fu      /* 128,   0, 128 */
    #define TFT_OLIVE       0x7BE0u      /* 128, 128,   0 */
    #define TFT_LIGHTGREY   0xC618u      /* 192, 192, 192 */
    #define TFT_DARKGREY    0x7BEFu      /* 128, 128, 128 */
    #define TFT_BLUE        0x001Fu      /*   0,   0, 255 */
    #define TFT_GREEN       0x07E0u      /*   0, 255,   0 */
    #define TFT_CYAN        0x07FFu      /*   0, 255, 255 */
    #define TFT_RED         0xf800u      /* 255,   0,   0 */
    #define TFT_MAGENTA     0xF81Fu      /* 255,   0, 255 */
    #define TFT_YELLOW      0xFFE0u      /* 255, 255,   0 */
    #define TFT_WHITE       0xFFFFu      /* 255, 255, 255 */
    #define TFT_ORANGE      0xFD20u      /* 255, 165,   0 */
    #define TFT_GREENYELLOW 0xAFE5u      /* 173, 255,  47 */
    #define TFT_PINK        0xF81Fu

    #define LINE0           0
    #define LINE1           8
    #define LINE2           16
    #define LINE3           24
    #define LINE4           32
    #define LINE5           40
    #define LINE6           48
    #define LINE7           56
    #define LINE8           64
    #define LINE9           72
    #define LINE10          80

    #define TFT_FLIP(C) (C >> 8 | C << 8)

    extern spi_transaction_t trans[6];

    extern uint16_t lcdHeight;
    extern uint16_t lcdWidth;
    extern spi_device_handle_t tft_spi;

    bool tft_init(type_lcd_t display, uint16_t height, uint16_t width);
    void tft_send_lines(int ypos, uint16_t *linedata);
    void tft_setWindow(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
    void tft_clear(uint16_t color);
    void tft_set_madctl(uint8_t m);
    void tft_setFG(const uint16_t c);
    void tft_setBG(const uint16_t c);
    void tft_setTextFillBG();
    void tft_resetTextFillBG();
    void tft_fastPixel(uint8_t x, uint8_t y);
    void send_line_finish(spi_device_handle_t spi);
    void tft_line(int x0, int y0, int x1, int y1);
    void tft_putc(const char c, const uint8_t x, const uint8_t y);
    void tft_puts(char *str, uint8_t x, uint8_t y);
    void tft_printf(const uint8_t x, const uint8_t y, const char *format, ...);

    void tft_drawBuffer(uint16_t *pixels);
    void tft_clearBuffer(uint16_t *pixels);
    void tft_setPixelBuffer(uint16_t *pixels, uint8_t x, uint8_t y);
    void tft_lineBuffer(uint16_t *pixels, int x0, int y0, int x1, int y1);
    void tft_putcBuffer(uint16_t *pixels, const char c, const uint8_t x, const uint8_t y);
    void tft_putsBuffer(uint16_t *pixels, char *str, uint8_t x, uint8_t y);
    void tft_printfBuffer(uint16_t *pixels, const uint8_t x, const uint8_t y, const char *format, ...);
    uint16_t tft_color565(uint16_t r, uint16_t g, uint16_t b);
    uint16_t tft_hue2RGB(uint16_t hue, uint8_t bight);

    #ifdef __cplusplus
    }
    #endif
#endif
