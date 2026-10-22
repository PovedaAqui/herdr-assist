/* Framebuffer de 1 bit do pager — C puro, compilável no host. */

#include <string.h>

#include "epd_bsp.h"

static uint8_t s_fb[EPD_FB_BYTES];

uint8_t *epd_fb(void) { return s_fb; }

void epd_fb_clear(void) { memset(s_fb, 0x00, sizeof(s_fb)); }

void epd_fb_pixel(int x, int y, bool black)
{
    if (x < 0 || y < 0 || x >= EPD_W || y >= EPD_H) return;
    uint8_t *b = &s_fb[(y * EPD_W + x) >> 3];
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    if (black) *b |= m; else *b &= (uint8_t)~m;
}

bool epd_fb_get(int x, int y)
{
    if (x < 0 || y < 0 || x >= EPD_W || y >= EPD_H) return false;
    return (s_fb[(y * EPD_W + x) >> 3] & (0x80u >> (x & 7))) != 0;
}

void epd_fb_rect(int x, int y, int w, int h, bool black)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            epd_fb_pixel(i, j, black);
}

void epd_fb_hline(int x, int y, int w, bool black)
{
    for (int i = x; i < x + w; i++) epd_fb_pixel(i, y, black);
}

void epd_fb_invert_rect(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            epd_fb_pixel(i, j, !epd_fb_get(i, j));
}
