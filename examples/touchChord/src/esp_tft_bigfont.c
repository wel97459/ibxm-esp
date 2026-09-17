#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_tft_tile.h"

const uint8_t tiles_bigfont_decode[]={
    0x0f, 0x0f, // Space
    0x00, 0x10, // 0
    0x01, 0x11, // 1
    0x02, 0x12, // 2
    0x03, 0x13, // 3
    0x04, 0x14, // 4
    0x05, 0x15, // 5
    0x06, 0x16, // 6
    0x07, 0x17, // 7
    0x08, 0x18, // 8
    0x09, 0x19, // 9


    0x0a,0x1a, //A
    0x0b,0x1b, //B
    0x0c,0x1c, //C
    0x0d,0x1d, //D
    0x0e,0x1e, //E
    0x0e,0x1f, //F
    0x0c,0x30, //G
    0x21,0x31, //H
    0x22,0x11, //I
    0x23,0x33, //J
    0x24,0x34, //K
    0x25,0x35, //L
    0x26,0x36, //M
    0x27,0x37, //N
    0x09,0x18, //O
    0x09,0x38, //P
    0x09,0x39, //Q
    0x09,0x3a, //R
    0x2b,0x3b, //S
    0x2c,0x11, //T
    0x2d,0x18, //U
    0x2d,0x3d, //V
    0x2e,0x3e, //W
    0x2f,0x3f, //X
    0x40,0x11, //Y
    0x41,0x51, //Z


    0x22,0x50, //!
    0x42,0x52, //?
    0x46,0x56, //+
    0x47,0x57, //-
    0x4c,0x5c, //(
    0x4d,0x5d, //)
    0x0f,0x88, //.
    0x0f,0x89, //,
    0x88,0x88, //:
    0x88,0x89, //;
    0x3c,0x0f, //~
    0x4e,0x5e, //<
    0x4f,0x5f //>
};

// Generated from bigfont.chr (see bigfont_data.c) — 144 tiles * 24 bytes.
extern const uint8_t tiles_bigfont[];

void tft_bigfont_putc(const uint8_t x, const uint8_t y, const uint8_t pal, const char c)
{
    char cc = c;
    if(cc >= '0' && cc <= '9') {
        cc -= '0';
        cc+=1;
    } else if(cc >= 'A' && cc <= 'Z'){
        cc -= 'A';
        cc+=1+10;
    } else if(cc >= 'a' && cc <= 'z'){
        cc -= 'a';
        cc+=1+10;
    } else if(cc == ' ') {
        cc=0;
    } else if(cc == '!') {
        cc=1+10+26;
    } else if(cc == '?') {
        cc=1+10+26+1;
    } else if(cc == '+') {
        cc=1+10+26+2;
    } else if(cc == '-') {
        cc=1+10+26+3;
    } else if(cc == '(') {
        cc=1+10+26+4;
    } else if(cc == ')') {
        cc=1+10+26+5;
    } else if(cc == '.') {
        cc=1+10+26+6;
    } else if(cc == ',') {
        cc=1+10+26+7;
    } else if(cc == ':') {
        cc=1+10+26+8;
    } else if(cc == ';') {
        cc=1+10+26+9;
    } else if(cc == '~') {
        cc=1+10+26+10;
    } else if(cc == '<') {
        cc=1+10+26+11;
    } else if(cc == '>') {
        cc=1+10+26+12;
    } else cc=0;

//    printf("%c, %u:%u, %u:%u\n", c, (cc<<1),tiles_bigfont_decode[(cc<<1)],(cc<<1)+1,tiles_bigfont_decode[(cc<<1)+1]);
    if (FILP_V_TILE & pal) {
        tile_map[y][x].tile = &tiles_bigfont[tiles_bigfont_decode[(cc<<1)+1]*24];
        tile_map[y][x].palette = pal;
        tile_map[y+1][x].tile = &tiles_bigfont[tiles_bigfont_decode[(cc<<1)]*24];
        tile_map[y+1][x].palette = pal;
    }else{
        tile_map[y][x].tile = &tiles_bigfont[tiles_bigfont_decode[(cc<<1)]*24];
        tile_map[y][x].palette = pal;
        tile_map[y+1][x].tile = &tiles_bigfont[tiles_bigfont_decode[(cc<<1)+1]*24];
        tile_map[y+1][x].palette = pal;
    }
}

void tft_bigfont_puts(const uint8_t x, const uint8_t y, const uint8_t pal, char *str)
{
    uint8_t xx = x, yy = y;
    char *s = str;
    while(*s != 0){
        if(*s == '\n'){
            yy+=2;
            xx=0;
            s++;
        }else{
		    tft_bigfont_putc(xx++, yy, pal, *s++);
        }
	}
}

void tft_bigfont_printf(const uint8_t x, const uint8_t y, const uint8_t pal, const char *format, ...)
{
    va_list arg;
    va_start(arg, format);
    char buff[64];
    vsprintf(buff, format, arg);
    tft_bigfont_puts(x, y, pal, buff);
    va_end(arg);
}

void tft_lilfont_putc(const uint8_t x, const uint8_t y, const uint8_t pal, const char c)
{
    char cc = c;
    if(cc >= '0' && cc <= '9') {
        cc -= '0';
        cc+=0x60;
    } else if(cc >= 'A' && cc <= 'Z'){
        cc -= 'A';
        cc+=0x6a;
    } else if(cc >= 'a' && cc <= 'z'){
        cc -= 'a';
        cc+=0x6a;
    } else if(cc == ' ') {
        cc=0x0f;
    } else if(cc == '!') {
        cc=0x84;
    } else if(cc == '?') {
        cc=0x85;
    } else if(cc == '+') {
        cc=0x86;
    } else if(cc == '-') {
        cc=0x87;
    } else if(cc == '(') {
        cc=0x8a;
    } else if(cc == ')') {
        cc=0x8b;
    } else if(cc == '.') {
        cc=0x88;
    } else if(cc == ',') {
        cc=0x89;
    } else if(cc == ':') {
        cc=0x8c;
    } else cc=0x0f;

//    printf("%c, %u:%u, %u:%u\n", c, (cc<<1),tiles_bigfont_decode[(cc<<1)],(cc<<1)+1,tiles_bigfont_decode[(cc<<1)+1]);
    tile_map[y][x].tile = &tiles_bigfont[cc*24];
    tile_map[y][x].palette = pal;

}

void tft_lilfont_puts(const uint8_t x, const uint8_t y, const uint8_t pal, char *str)
{
    uint8_t xx = x, yy = y;
    char *s = str;
    while(*s != 0){
        if(*s == '\n'){
            yy+=2;
            xx=0;
            s++;
        }else{
		    tft_lilfont_putc(xx++, yy, pal, *s++);
        }
	}
}

void tft_lilfont_printf(const uint8_t x, const uint8_t y, const uint8_t pal, const char *format, ...)
{
    va_list arg;
    va_start(arg, format);
    char buff[64];
    vsprintf(buff, format, arg);
    tft_lilfont_puts(x, y, pal, buff);
    va_end(arg);
}


void tft_bigfont_win_putc(const uint8_t x, const uint8_t y, const uint8_t pal, const char c)
{
    char cc = c;
    if(cc >= '0' && cc <= '9') {
        cc -= '0';
        cc+=1;
    } else if(cc >= 'A' && cc <= 'Z'){
        cc -= 'A';
        cc+=1+10;
    } else if(cc >= 'a' && cc <= 'z'){
        cc -= 'a';
        cc+=1+10;
    } else if(cc == ' ') {
        cc=0;
    } else if(cc == '!') {
        cc=1+10+26;
    } else if(cc == '?') {
        cc=1+10+26+1;
    } else if(cc == '+') {
        cc=1+10+26+2;
    } else if(cc == '-') {
        cc=1+10+26+3;
    } else if(cc == '(') {
        cc=1+10+26+4;
    } else if(cc == ')') {
        cc=1+10+26+5;
    } else if(cc == '.') {
        cc=1+10+26+6;
    } else if(cc == ',') {
        cc=1+10+26+7;
    } else if(cc == ':') {
        cc=1+10+26+8;
    } else if(cc == ';') {
        cc=1+10+26+9;
    } else if(cc == '~') {
        cc=1+10+26+10;
    } else cc=0;

//    printf("%c, %u:%u, %u:%u\n", c, (cc<<1),tiles_bigfont_decode[(cc<<1)],(cc<<1)+1,tiles_bigfont_decode[(cc<<1)+1]);

    tile_map_window[y][x].tile = &tiles_bigfont[tiles_bigfont_decode[(cc<<1)]*24];
    tile_map_window[y][x].palette = pal;
    tile_map_window[y+1][x].tile = &tiles_bigfont[tiles_bigfont_decode[(cc<<1)+1]*24];
    tile_map_window[y+1][x].palette = pal;
}

void tft_bigfont_win_puts(const uint8_t x, const uint8_t y, const uint8_t pal, char *str)
{
    uint8_t xx = x, yy = y;
    char *s = str;
    while(*s != 0){
        if(*s == '\n'){
            yy+=2;
            xx=0;
            s++;
        }else{
		    tft_bigfont_win_putc(xx++, yy, pal, *s++);
        }
	}
}

void tft_bigfont_win_printf(const uint8_t x, const uint8_t y, const uint8_t pal, const char *format, ...)
{
    va_list arg;
    va_start(arg, format);
    char buff[64];
    vsprintf(buff, format, arg);
    tft_bigfont_win_puts(x, y, pal, buff);
    va_end(arg);
}
