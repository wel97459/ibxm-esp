#ifndef ESP_TTF_TILE
#define  ESP_TTF_TILE

    #ifdef __cplusplus
    extern "C" {
    #endif

    #define DIS_TILE_HEIGHT             240
    #define DIS_TILE_WIDTH              240

    #define BUFF_SECION_BITS            30720 //DIS_TILE_WIDTH*2*8*8;
    #define BUFF_SECION_SIZE            3840  //DIS_TILE_WIDTH*8*2
    #define BUFF_SECION_HALF            1920
    #define FILP_H_TILE                 0x80
    #define FILP_V_TILE                 0x40
    #define TRANSPARENT_TILE            0x20
    #define PALETTE_BITS_TILE           0x1F

    typedef struct {
        //7 = Flip H Tile
        //6 = Flip V Tile
        //5 = Transparent
        //0-4 = Palette Sellect
        uint8_t palette;
        uint8_t *tile;
    } tile_t;

    tile_t tile_map[61][61];
    tile_t tile_map_window[30][30];
    tile_t tile_map_bg[30][30];
    uint16_t palettes[32][4];

    uint16_t tile_mapSection[BUFF_SECION_SIZE];

    void tft_tile_init();
    void tft_tile_clearPalette();
    void tft_tile_clearWindow();
    void tft_tile_clear();
    void tft_tile_sendLine(const uint16_t ypos, const uint8_t field);
    void tft_tile_render(const uint8_t ypos, const uint16_t xo, const uint16_t yo);
    void tft_tile_putPalette(const uint8_t pal, const uint16_t p0, const uint16_t p1, const uint16_t p2, const uint16_t p3);
    void tft_tile_putPaletteSingle(const uint8_t pal, const uint8_t p, const uint16_t p0);
    void rotate_left_pal4(const uint8_t pal);
    void rotate_right_pal4(const uint8_t pal);
    void rotate_left_pal7(const uint8_t pal);
    void rotate_right_pal7(const uint8_t pal);
    void tft_tile_putGFX(const uint8_t x, const uint8_t y, const uint8_t pal, const uint8_t *tile);
    void tft_tile_putWindowGFX(const uint8_t x, const uint8_t y, const uint8_t pal, const uint8_t *tile);
    void tft_tile_putBackgoundGFX(const uint8_t x, const uint8_t y, const uint8_t pal, const uint8_t *tile);

    #ifdef __cplusplus
    }
    #endif
#endif
//01010101
//00110011
