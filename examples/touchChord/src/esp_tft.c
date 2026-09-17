#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_tft.h"
#include "font.h"

#define  INIT_END 0xFF
#define  INIT_DELAY 0x80

#define FLIP_BIT_ON(X, Y)				X |= (1<<Y)
#define FLIP_BIT_OFF(X, Y)				X &= ~(1<<Y)

static const  char *TAG = "ESP_LCD";

// Global state (defined here; declared extern in the header so C++ TU can link).
spi_transaction_t trans[6];
uint16_t lcdWidth  = 240;
uint16_t lcdHeight = 240;
spi_device_handle_t tft_spi = NULL;

uint16_t tile[64];

uint16_t fgColor;
uint16_t bgColor;
uint8_t textSetting;


typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} init_cmd_list_t;


DRAM_ATTR static const init_cmd_list_t none_init_cmds[]={
    {0, {0}, INIT_END} //End of init
};

DRAM_ATTR static const init_cmd_list_t ili9163_init_cmds[]={
    {TFT_SWRESET, {0}, 0x00}, // Soft Reset
    {0x11, {0}, 0x00}, // Exit sleep mode

    {TFT_COLMOD, {0x05}, 0x01}, // Set pixel format
    {TFT_GAMSET, {0x04}, 0x01}, // Set Gamma curve
    {0xF2, {0x01}, 0x01}, // Gamma adjustment enabled
    {TFT_PVGAMCTRL, {0x3F, 0x25, 0x1C, 0x1E, 0x20, 0x12, 0x2A, 0x90,
	        0x24, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x0F}, // Positive Gamma
    {TFT_NVGAMCTRL, {0x20, 0x20, 0x20, 0x20, 0x05, 0x00, 0x15, 0xA7,
	        0x3D, 0x18, 0x25, 0x2A, 0x2B, 0x2B, 0x3A}, 0x0F}, // Negative Gamma
    {0xB1, {0x08, 0x08}, 0x02}, // Frame rate control 1
    {0xB4, {0x07}, 0x01}, // Display inversion
    {TFT_LCMCTRL, {0x0a, 0x02}, 0x01}, // Power control 1
    {0xC1, {0x02}, 0x01}, // Power control 2
    {TFT_VCMOFSET, {0x50, 0x5B}, 0x02}, // Vcom control 1
    {0xC7, {0x40}, 0x01}, // Vcom offset
    {TFT_CASET, {0x00, 0x00, 0x00, 0x7F}, 0x04 | INIT_DELAY}, // Set column address
    {TFT_RASET, {0x00, 0x00, 0x00, 0x9F}, 0x04}, // Set page address
    {TFT_MADCTL, {0xC8}, 0x01}, // Set address mode
    {TFT_DISPON, {0}, 0x00}, // Set display on
    {0, {0}, INIT_END} //End of init
};

//Place data into DRAM. Constant data gets placed into DROM by default, which is not accessible by DMA.
DRAM_ATTR static const init_cmd_list_t st7789_init_cmds[]={
    {TFT_SWRESET, {0}, INIT_DELAY}, // Sleep out
    {TFT_SLPOUT, {0}, INIT_DELAY}, // Sleep out
    {TFT_COLMOD, {0x55}, 0x01 | INIT_DELAY},
    {TFT_MADCTL, {0x00}, 1},
    {TFT_PVGAMCTRL, {0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0e, 0x12, 0x14, 0x17}, 14},
    {TFT_NVGAMCTRL, {0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31, 0x54, 0x47, 0x0e, 0x1c, 0x17, 0x1b, 0x1e}, 14},
    {TFT_CASET, {0x00, 0x00, 0x00, 0xf0}, 4},
    {TFT_RASET, {0x00, 0x00, 0x00, 0xf0}, 4},
    {TFT_CMD2EN, {0x5a, 0x69, 0x02, 0x01}, 4},
    {TFT_FRCTR2, {0x01}, 1},
    {TFT_INVON, {0}, INIT_DELAY},
    {TFT_NORON, {0}, INIT_DELAY},
    {TFT_DISPON, {0}, INIT_DELAY},
    {0, {0}, INIT_END}
};

static void tft_cmd(spi_device_handle_t spi, const uint8_t cmd)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=8;                     //Command is 8 bits
    t.tx_buffer=&cmd;               //The data is the cmd itself
    t.user=(void*)0;                //D/C needs to be set to 0
    ret=spi_device_polling_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);            //Should have had no issues.
}

static void tft_data(spi_device_handle_t spi, const uint8_t *data, int len)
{
    esp_err_t ret;
    spi_transaction_t t;
    if (len==0) return;             //no need to send anything
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length=len*8;                 //Len is in bytes, transaction length is in bits.
    t.tx_buffer=data;               //Data
    t.user=(void*)1;                //D/C needs to be set to 1
    ret=spi_device_polling_transmit(spi, &t);  //Transmit!
    assert(ret==ESP_OK);            //Should have had no issues.
}

static void tft_spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc=(int)t->user;
    gpio_set_level(PIN_NUM_DC, dc);
}

//Initialize the display. Returns true on success; on any SPI/bus error it logs
//and returns false instead of panicking, so a missing/incorrect panel can never
//brick the audio firmware (the display is purely informational output).
bool tft_init(type_lcd_t display, uint16_t height, uint16_t width)
{
    lcdWidth  = width;
    lcdHeight = height;

    fgColor = TFT_WHITE;
    bgColor = TFT_BLACK;
    textSetting = 0;
    //Initialize SPI
    esp_err_t ret;
    spi_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_MISO,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=PARALLEL_LINES*lcdWidth*2
    };

    spi_device_interface_config_t devcfg={
        .clock_speed_hz=10*1000*1000,           // 10 MHz: reliable over jumper wires (was 26 MHz)
        .mode=0,                            //SPI mode 0
        .spics_io_num=PIN_NUM_CS,               //CS pin
        .queue_size=7,                          //We want to be able to queue 7 transactions at a time
        .pre_cb=tft_spi_pre_transfer_callback,  //Specify pre-transfer callback to handle D/C line
    };
    //Initialize the SPI bus
    ret=spi_bus_initialize(HSPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) { ESP_LOGE("ESP_LCD", "spi_bus_initialize failed: %s", esp_err_to_name(ret)); return false; }

    //Attach the LCD to the SPI bus
    ret=spi_bus_add_device(HSPI_HOST, &devcfg, &tft_spi);
    if (ret != ESP_OK) { ESP_LOGE("ESP_LCD", "spi_bus_add_device failed: %s", esp_err_to_name(ret)); return false; }

    //Initialize non-SPI GPIOs
    gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);

    //Reset the display
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(100 / portTICK_RATE_MS);
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(100 / portTICK_RATE_MS);

    int cmd=0;

    const init_cmd_list_t* tft_init_cmds;
    tft_init_cmds = none_init_cmds;
    if(display == LCD_TYPE_ST7789)
        tft_init_cmds = st7789_init_cmds;
    else if(display == LCD_TYPE_ILI9163)
        tft_init_cmds = ili9163_init_cmds;

    //Send all the commands
    while (tft_init_cmds[cmd].databytes!=INIT_END) {
        tft_cmd(tft_spi, tft_init_cmds[cmd].cmd);
        tft_data(tft_spi, tft_init_cmds[cmd].data, tft_init_cmds[cmd].databytes&0x1F);
        if (tft_init_cmds[cmd].databytes&INIT_DELAY) {
            vTaskDelay(100 / portTICK_RATE_MS);
        }
        cmd++;
    }

    memset(&trans[0], 0, sizeof(spi_transaction_t));
    memset(&trans[1], 0, sizeof(spi_transaction_t));
    memset(&trans[2], 0, sizeof(spi_transaction_t));
    memset(&trans[3], 0, sizeof(spi_transaction_t));
    memset(&trans[4], 0, sizeof(spi_transaction_t));
    memset(&trans[5], 0, sizeof(spi_transaction_t));

    //In theory, it's better to initialize trans and data only once and hang on to the initialized
    //variables. We allocate them on the stack, so we need to re-init them each call.
    for (uint8_t x=0; x<6; x++) {
        if ((x&1)==0) {
            //Even transfers are commands
            trans[x].length=8;
            trans[x].user=(void*)0;
        } else {
            //Odd transfers are data
            trans[x].length=32;
            trans[x].user=(void*)1;
        }
        trans[x].flags=SPI_TRANS_USE_TXDATA;
    }
    trans[0].tx_data[0]=TFT_CASET;      //Column Address Set
    trans[1].tx_data[0]=0;                  //Start Col High
    trans[1].tx_data[1]=0;                  //Start Col Low
    trans[1].tx_data[2]=0;                  //End Col High
    trans[1].tx_data[3]=0;                  //End Col Low
    trans[2].tx_data[0]=TFT_RASET;      //Page address set
    trans[3].tx_data[0]=0;                  //Start page high
    trans[3].tx_data[1]=0;                  //start page low
    trans[3].tx_data[2]=0;                  //end page high
    trans[3].tx_data[3]=0;                  //end page low
    trans[4].tx_data[0]=TFT_RAMWR;      //memory write
    trans[5].flags=0;                       //undo SPI_TRANS_USE_TXDATA flag
    return true;
}

/* To send a set of lines we have to send a command, 2 data bytes, another command, 2 more data bytes and another command
 * before sending the line data itself; a total of 6 transactions. (We can't put all of this in just one transaction
 * because the D/C line needs to be toggled in the middle.)
 * This routine queues these commands up as interrupt transactions so they get
 * sent faster (compared to calling spi_device_transmit several times), and at
 * the mean while the lines for next transactions can get calculated.
 */
void tft_send_lines(int ypos, uint16_t *linedata)
{
    esp_err_t ret;
    trans[1].tx_data[0]=0;                  //Start Col High
    trans[1].tx_data[1]=0;                  //Start Col Low
    trans[1].tx_data[2]=lcdWidth >> 8;       //End Col High
    trans[1].tx_data[3]=lcdWidth & 0x00ff;     //End Col Low
    trans[3].tx_data[0]=ypos >> 8;        //Start page high
    trans[3].tx_data[1]=ypos & 0x00ff;      //start page low
    trans[3].tx_data[2]=(ypos+PARALLEL_LINES) >> 8;    //end page high
    trans[3].tx_data[3]=(ypos+PARALLEL_LINES) & 0x00ff;  //end page low
    trans[5].tx_buffer=linedata;        //finally send the line data
    trans[5].length=lcdWidth*PARALLEL_LINES*2*8;          //Data length, in bits
    //Queue all transactions.
    for (uint8_t x=0; x<6; x++) {
        ret=spi_device_queue_trans(tft_spi, &trans[x], portMAX_DELAY);
        assert(ret==ESP_OK);
    }

    //When we are here, the SPI driver is busy (in the background) getting the transactions sent. That happens
    //mostly using DMA, so the CPU doesn't have much to do here. We're not going to wait for the transaction to
    //finish because we may as well spend the time calculating the next line. When that is done, we can call
    //send_line_finish, which will wait for the transfers to be done and check their status.
}


void tft_setWindow(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    // Faster range checking, possible because x and y are unsigned
    if ((x >= lcdWidth) || (y >= lcdHeight)) return;
    if ((w >= lcdWidth) || (h >= lcdHeight)) return;

    //tft_send_line_finish();

    esp_err_t ret;

    trans[1].tx_data[0]=0;       //Start Col High
    trans[1].tx_data[1]=x;       //Start Col Low
    trans[1].tx_data[2]=0;       //Start Col High
    trans[1].tx_data[3]=w;       //Start Col Low

    trans[3].tx_data[0]=0;    //Start page high
    trans[3].tx_data[1]=y;    //start page low
    trans[3].tx_data[2]=0;    //end page high
    trans[3].tx_data[3]=h;    //end page low

    for (uint8_t x=0; x<6; x++) {
        ret=spi_device_queue_trans(tft_spi, &trans[x], portMAX_DELAY);
        assert(ret==ESP_OK);
    }

}

void tft_setFG(const uint16_t c) {fgColor = TFT_FLIP(c);}
void tft_setBG(const uint16_t c) {bgColor = TFT_FLIP(c);}

void tft_setTextFillBG() {FLIP_BIT_ON(textSetting, 0);}
void tft_resetTextFillBG() {FLIP_BIT_OFF(textSetting, 0);}

void tft_fastPixel(uint8_t x, uint8_t y)
{
    // Faster range checking, possible because x and y are unsigned
    if ((x >= lcdWidth) || (y >= lcdHeight)) return;
    esp_err_t ret;

    memset(&trans[5], 0, sizeof(spi_transaction_t));

    trans[5].tx_buffer = &fgColor;    //Start page high
    trans[5].length=16; //Data length, in bits
    trans[5].flags=0;
    trans[5].user=(void*)1;

    tft_setWindow(x, y, x, y);

}


void tft_line(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;

    while (tft_fastPixel(x0, y0), x0 != x1 || y0 != y1) {
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

void tft_putc(const char c, const uint8_t x, const uint8_t y)
{
    // Faster range checking, possible because x and y are unsigned
    if ((x >= lcdWidth) || (y >= lcdHeight)) return;
    esp_err_t ret;

    memset(&trans[5], 0, sizeof(spi_transaction_t));

    uint8_t line, i, ii, t = 0;

    for(i = 0; i < 8; i++) {
        line = font[(c << 3) + i];
        for( ii = 0; ii < 8; ii++) {
            tile[t++] = ((line << ii) & 0x80) > 0 ? fgColor : bgColor;
        }
    }

    trans[5].tx_buffer = tile;    //Start page high
    trans[5].length=64*16; //Data length, in bits
    trans[5].flags=0;
    trans[5].user=(void*)1;
    tft_setWindow(x, y, x+7, y+7);
}


void tft_puts(char *str, uint8_t x, uint8_t y)
{
    while(*str != 0){
		tft_putc(*str++, x, y);
		x += 8;
	}
}

void tft_printf(const uint8_t x, const uint8_t y, const char *format, ...)
{
    va_list arg;
    va_start(arg, format);
    char buff[128];
    vsprintf(buff, format, arg);
    tft_puts(buff, x, y);
    va_end(arg);
}

void send_line_finish(spi_device_handle_t spi)
{
    spi_transaction_t *rtrans;
    esp_err_t ret;
    //Wait for all 6 transactions to be done and get back the results.
    for (int x=0; x<6; x++) {
        ret=spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
        assert(ret==ESP_OK);
        //We could inspect rtrans now if we received any info back. The LCD is treated as write-only, though.
    }
}


void tft_drawBuffer(uint16_t *pixels)
{

    for (int y=0; y<lcdHeight; y+=PARALLEL_LINES) {
        //Finish up the sending process of the previous line, if any
        //Send the line we currently calculated.
        tft_send_lines(y, &pixels[lcdWidth * y]);
        send_line_finish(tft_spi);
        //The line set is queued up for sending now; the actual sending happens in the
        //background. We can go on to calculate the next line set as long as we do not
        //touch line[sending_line]; the SPI sending process is still reading from that.
    }
}

void tft_clearBuffer(uint16_t *pixels)
{
    for (size_t i = 0; i < lcdWidth * lcdHeight; i++) {
        pixels[i] = bgColor;
    }
}

// Set display orientation/color-order register (MADCTL) on the fly.
void tft_set_madctl(uint8_t m)
{
    tft_cmd(tft_spi, TFT_MADCTL);
    tft_data(tft_spi, &m, 1);
}

// Fill the whole screen with one color (no framebuffer needed).
void tft_clear(uint16_t color)
{
    esp_err_t ret;
    // Set the full-screen window once (CASET 0..W-1, RASET 0..H-1, then RAMWR).
    memset(&trans[0], 0, sizeof(spi_transaction_t));
    memset(&trans[1], 0, sizeof(spi_transaction_t));
    memset(&trans[2], 0, sizeof(spi_transaction_t));
    memset(&trans[3], 0, sizeof(spi_transaction_t));
    memset(&trans[4], 0, sizeof(spi_transaction_t));
    memset(&trans[5], 0, sizeof(spi_transaction_t));

    trans[0].length = 8;  trans[0].user = (void*)0; trans[0].flags = SPI_TRANS_USE_TXDATA;
    trans[1].length = 32; trans[1].user = (void*)1; trans[1].flags = SPI_TRANS_USE_TXDATA;
    trans[2].length = 8;  trans[2].user = (void*)0; trans[2].flags = SPI_TRANS_USE_TXDATA;
    trans[3].length = 32; trans[3].user = (void*)1; trans[3].flags = SPI_TRANS_USE_TXDATA;
    trans[4].length = 8;  trans[4].user = (void*)0; trans[4].flags = SPI_TRANS_USE_TXDATA;

    trans[0].tx_data[0] = TFT_CASET;
    trans[1].tx_data[0]=0; trans[1].tx_data[1]=0;
    trans[1].tx_data[2]=lcdWidth >> 8;  trans[1].tx_data[3]=lcdWidth & 0x00ff;
    trans[2].tx_data[0] = TFT_RASET;
    trans[3].tx_data[0]=0; trans[3].tx_data[1]=0;
    trans[3].tx_data[2]=lcdHeight >> 8; trans[3].tx_data[3]=lcdHeight & 0x00ff;
    trans[4].tx_data[0] = TFT_RAMWR;

    // Queue the 5 window-setup transactions together, then wait for exactly those 5.
    for (uint8_t x = 0; x < 5; x++) {
        ret = spi_device_queue_trans(tft_spi, &trans[x], portMAX_DELAY);
        assert(ret == ESP_OK);
    }
    for (uint8_t x = 0; x < 5; x++) {
        spi_transaction_t *rtrans;
        ret = spi_device_get_trans_result(tft_spi, &rtrans, portMAX_DELAY);
        assert(ret == ESP_OK);
    }

    // Stream one row of `color` per line; the LCD auto-increments the address.
    // Use spi_device_transmit (blocking, one transaction) so we never rely on
    // send_line_finish()'s 6-result assumption and can never deadlock.
    uint16_t row[lcdWidth];
    for (int i = 0; i < lcdWidth; i++) row[i] = color;
    trans[5].tx_buffer = row;
    trans[5].length    = lcdWidth * 16;   // 240 px * 16 bit
    trans[5].flags     = 0;
    trans[5].user      = (void*)1;
    for (int y = 0; y < lcdHeight; y++) {
        ret = spi_device_transmit(tft_spi, &trans[5]);
        assert(ret == ESP_OK);
    }
}

void tft_setPixelBuffer(uint16_t *pixels, uint8_t x, uint8_t y)
{
    // Faster range checking, possible because x and y are unsigned
    if ((x >= lcdWidth) || (y >= lcdHeight)) return;
    pixels[x + (y * lcdWidth)] = fgColor;
}


void tft_lineBuffer(uint16_t *pixels, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;

    while (tft_setPixelBuffer(pixels, x0, y0), x0 != x1 || y0 != y1) {
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

void tft_putcBuffer(uint16_t *pixels, const char c, const uint8_t x, const uint8_t y)
{
    // Faster range checking, possible because x and y are unsigned
    if ((x >= lcdWidth) || (y >= lcdHeight)) return;
    uint8_t flip = c > 127 ? 1 : 0;
    uint8_t line, i, ii;
    uint8_t cc = c > 127 ? c - 128 : c;
    for(i = 0; i < 8; i++) {
        line = font[(cc << 3) + i];
        for( ii = 0; ii < 8; ii++) {
            if(!flip){
                if((line << ii) & 0x80){
                    pixels[x+ii + ((y+i) * lcdWidth)] = fgColor;
                } else if(textSetting & 0x01){
                    pixels[x+ii + ((y+i) * lcdWidth)] = bgColor;
                }
            }else{
                if((line >> ii) & 0x01){
                    pixels[x+ii + ((y+i) * lcdWidth)] = fgColor;
                } else if(textSetting & 0x01){
                    pixels[x+ii + ((y+i) * lcdWidth)] = bgColor;
                }
            }
        }
    }
}


void tft_putsBuffer(uint16_t *pixels, char *str, uint8_t x, uint8_t y)
{
    while(*str != 0){
		tft_putcBuffer(pixels, *str++, x, y);
		x += 8;
	}
}

void tft_printfBuffer(uint16_t *pixels, const uint8_t x, const uint8_t y, const char *format, ...)
{
    va_list arg;
    va_start(arg, format);
    char buff[64];
    vsprintf(buff, format, arg);
    tft_putsBuffer(pixels, buff, x, y);
    va_end(arg);
}

uint16_t tft_color565(uint16_t r, uint16_t g, uint16_t b)
{
    uint16_t v = 0;
    v |= ((r >> 3) << 11);
    v |= ((g >> 2) << 5);
    v |= ((b >> 3) << 0);
    //The LCD wants the 16-bit value in big-endian, so swap bytes
    return (v >> 8) | (v << 8);
}

uint16_t tft_hue2RGB(uint16_t hue, uint8_t bight)
{
    uint8_t r=0,g=0,b=0;

	//hue 255 * 6 = 1024 range
	uint8_t hh = (hue >> 8) % 6; //hue divide by 256
	uint8_t h = (hue % 256) >> bight;
	uint8_t m = 255 >> bight;

	if(hh==0){
		r = m;
		g = h;
		b = 0;
	}

	if(hh==1){
		r = m - h;
		g = m;
		b = 0;
	}

	if(hh==2){
		r = 0;
		g = m;
		b = h;
	}

	if(hh==3){
		r = 0;
		g = m - h;
		b = m;
	}

	if(hh==4){
		r = h;
		g = 0;
		b = m;
	}

	if(hh==5){
		r = m;
		g = 0;
		b = m - h;
	}

	return tft_color565(r, g, b);
}
