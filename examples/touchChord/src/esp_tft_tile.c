#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
// #include "esp_spi_flash.h"
#include "esp_tft.h"
#include "esp_tft_tile.h"

//For right this is setup for a 240x240 display

// Global state defined here (declared extern in esp_tft_tile.h so C++ TU links).
tile_t tile_map[61][61];
tile_t tile_map_window[30][30];
tile_t tile_map_bg[30][30];
uint16_t palettes[32][4];
uint16_t tile_mapSection[BUFF_SECION_SIZE];

const uint8_t tiles_blank[] = {
    //Black
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

void tft_tile_sendLine(const uint16_t ypos, const uint8_t field)
{
    esp_err_t ret;
    trans[1].tx_data[0]=0;                  //Start Col High
    trans[1].tx_data[1]=0;                  //Start Col Low
    trans[1].tx_data[2]=(DIS_TILE_WIDTH)>>8;       //End Col High
    trans[1].tx_data[3]=(DIS_TILE_WIDTH)&0xff;     //End Col Low
    trans[3].tx_data[0]=ypos>>8;        //Start page high
    trans[3].tx_data[1]=ypos&0xff;      //start page low
    trans[3].tx_data[2]=(ypos+7)>>8;    //end page high
    trans[3].tx_data[3]=(ypos+7)&0xff;  //end page low
    trans[5].tx_buffer=&tile_mapSection[field ? BUFF_SECION_HALF : 0];        //finally send the line data
    trans[5].length=BUFF_SECION_BITS;          //Data length, in bits

    //Queue all transactions.
    for (uint8_t x=0; x<6; x++) {
        ret=spi_device_queue_trans(tft_spi, &trans[x], portMAX_DELAY);
        assert(ret==ESP_OK);
    }
    //tft_send_line_finish();
}


void tft_tile_render(const uint8_t ypos, const uint16_t xo, const uint16_t yo)
{
    uint16_t *field_section, outpix;
    uint8_t pal, *tile, b; //has to do with tile palette
    uint8_t x, y, i; // for and next loops
    uint8_t ix, iy, ixi, iyi; //clean x and y cords without
    uint8_t tx, ty, ti, tt, tty; //current tile location
    uint8_t b0, b1, b2;
    field_section = &tile_mapSection[ypos & 0x01 ? 0 : BUFF_SECION_HALF];
    if(ypos>0) tft_tile_sendLine((ypos-1)*8, ypos & 0x01);

    iy=0;
    for (y = 0+(yo&0x07); y < 8+(yo&0x07); y++) {
        iyi = 7-iy;
        for (x = 0; x < 30; x++) {
            ix=0;
            for (i = 0+(xo&0x07); i < 8+(xo&0x07); i++) {
                ixi = 7-ix;
                outpix = palettes[0][0];

                tile = tile_map_bg[ypos][x].tile;
                if(tile){
                    pal = tile_map_bg[ypos][x].palette;

                    tt = (FILP_V_TILE & pal) ? iyi : iy;
                    ti = (FILP_H_TILE & pal) ? ixi : ix;

                    b2 = tile[tt+16];
                    tt = tt<<1;
                    b1 = tile[tt+1];
                    b0 = tile[tt];

                    pal += (0x80 & (b2<<ti)) >> 7;
                    b = (0x80 & (b1<<ti)) >> 6 | (0x80 & (b0<<ti)) >> 7;
                    outpix = palettes[PALETTE_BITS_TILE & pal][b];
                }

                tx = (x+(i>>3)+(xo>>3)) % 60;
                ty = (ypos+(y>>3)+(yo>>3)) % 60;
                tile = tile_map[ty][tx].tile;
                if(tile){
                    pal = tile_map[ty][tx].palette;

                    tt = (FILP_V_TILE & pal) ? 7 - (y & 0x07) : (y & 0x07);
                    ti = (FILP_H_TILE & pal) ? 7 - (i & 0x07) : (i & 0x07);

                    b2 = tile[tt+16];
                    tt = tt<<1;
                    b1 = tile[tt+1];
                    b0 = tile[tt];

                    pal += (0x80 & (b2<<ti)) >> 7;
                    b = (0x80 & (b1<<ti)) >> 6 | (0x80 & (b0<<ti)) >> 7;
                    if(b || (pal & TRANSPARENT_TILE) != TRANSPARENT_TILE){
                        outpix = palettes[PALETTE_BITS_TILE & pal][b];
                    }
                }

                tile = tile_map_window[ypos][x].tile;
                if(tile){
                    pal = tile_map_window[ypos][x].palette;

                    tt = (FILP_V_TILE & pal) ? iyi : iy;
                    ti = (FILP_H_TILE & pal) ? ixi : ix;

                    b2 = tile[tt+16];
                    tt = tt<<1;
                    b1 = tile[tt+1];
                    b0 = tile[tt];

                    pal += (0x80 & (b2<<ti)) >> 7;
                    b = (0x80 & (b1<<ti)) >> 6 | (0x80 & (b0<<ti)) >> 7;
                    if(b || (pal & TRANSPARENT_TILE) != TRANSPARENT_TILE){
                        outpix = palettes[PALETTE_BITS_TILE & pal][b];
                    }
                }
                field_section[ix  + (x << 3) + (iy * DIS_TILE_WIDTH)] = outpix;
                ix++;
            }
        }
        iy++;
    }
    if(ypos==29) tft_tile_sendLine((ypos)*8, 0);
}

void tft_tile_putPalette(const uint8_t pal, const uint16_t p0, const uint16_t p1, const uint16_t p2, const uint16_t p3)
{
    palettes[pal][0] = TFT_FLIP(p0);
    palettes[pal][1] = TFT_FLIP(p1);
    palettes[pal][2] = TFT_FLIP(p2);
    palettes[pal][3] = TFT_FLIP(p3);
}

void tft_tile_putPaletteSingle(const uint8_t pal, const uint8_t p, const uint16_t p0)
{
    palettes[pal][p & 0x03] = TFT_FLIP(p0);
}

void rotate_left_pal4(const uint8_t pal) {
    uint16_t t=0;
    t = palettes[pal][0];
    palettes[pal][0]=palettes[pal][1];
    palettes[pal][1]=palettes[pal][2];
    palettes[pal][2]=palettes[pal][3];
    palettes[pal][3]=t;
}

void rotate_right_pal4(const uint8_t pal) {
    uint16_t t=0;
    t = palettes[pal][3];
    palettes[pal][3]=palettes[pal][2];
    palettes[pal][2]=palettes[pal][1];
    palettes[pal][1]=palettes[pal][0];
    palettes[pal][0]=t;
}

void rotate_left_pal7(const uint8_t pal) {
    uint16_t t=0;
    t = palettes[pal][1];
    palettes[pal][1]=palettes[pal][2];
    palettes[pal][2]=palettes[pal][3];
    palettes[pal][3]=palettes[pal+1][0];
    palettes[pal+1][0]=palettes[pal+1][1];
    palettes[pal+1][1]=palettes[pal+1][2];
    palettes[pal+1][2]=palettes[pal+1][3];
    palettes[pal+1][3]=t;
}

void rotate_right_pal7(const uint8_t pal) {
    uint16_t t=0;
    t = palettes[pal+1][3];
    palettes[pal+1][3]=palettes[pal+1][2];
    palettes[pal+1][2]=palettes[pal+1][1];
    palettes[pal+1][1]=palettes[pal+1][0];
    palettes[pal+1][0]=palettes[pal][3];
    palettes[pal][3]=palettes[pal][2];
    palettes[pal][2]=palettes[pal][1];
    palettes[pal][1]=t;
}

void tft_tile_clearPalette()
{
    for (size_t i = 0; i < 32; i++) {
        for (size_t j = 0; j < 4; j++) {
            palettes[i][j] = 0;
        }
    }
}

void tft_tile_clearWindow()
{
    for (size_t i = 0; i < 30; i++) {
        for (size_t j = 0; j < 30; j++) {
                tile_map_window[i][j].tile = NULL;
                tile_map_bg[i][j].tile = NULL;
        }
    }
}

void tft_tile_clear()
{
    for (size_t i = 0; i < 60; i++) {
        for (size_t j = 0; j < 60; j++) {
            tile_map[i][j].palette = 0;
            tile_map[i][j].tile = NULL;
        }
    }
}

void tft_tile_init()
{
    tft_tile_clearPalette();
    tft_tile_clearWindow();
    tft_tile_clear();
    tft_tile_putPalette(0, TFT_BLACK, TFT_DARKGREY, TFT_LIGHTGREY, TFT_WHITE);
}

void tft_tile_putGFX(const uint8_t x, const uint8_t y, const uint8_t pal, const uint8_t *tile)
{
    tile_map[y% 60][x% 60].tile = tile;
    tile_map[y% 60][x% 60].palette = pal;
}

void tft_tile_putWindowGFX(const uint8_t x, const uint8_t y, const uint8_t pal, const uint8_t *tile)
{
    tile_map_window[y % 30][x % 30].tile = tile;
    tile_map_window[y % 30][x % 30].palette = pal;
}

void tft_tile_putBackgoundGFX(const uint8_t x, const uint8_t y, const uint8_t pal, const uint8_t *tile)
{
    tile_map_bg[y % 30][x % 30].tile = tile;
    tile_map_bg[y % 30][x % 30].palette = pal;
}
