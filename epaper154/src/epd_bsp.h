/* Painel do pager: framebuffer de 1 bit + driver SSD1681 do IDF.
 *
 * Duas metades, de propósito:
 *   - epd_fb.c: o framebuffer e as primitivas de desenho. C puro, sem nada do
 *     IDF — é o que o teste de host (scripts/pager_ui_test.c) linka para gerar
 *     a tela como imagem sem placa nenhuma;
 *   - epd_bsp.c: o vidro. Usa o componente oficial espressif/esp_lcd_ssd1681
 *     (Apache-2.0, baixado pelo component manager) — nenhum driver de
 *     terceiro vendorizado.
 *
 * Pinos (user_config.h dos exemplos oficiais da Waveshare, iguais em V1/V2):
 *   EPD_PWR=6 BUSY=8 RST=9 DC=10 CS=11 SCK=12 MOSI=13 — ver board_pins.h.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_W 200
#define EPD_H 200
#define EPD_FB_BYTES ((EPD_W * EPD_H) / 8)   /* 5000 bytes */

/* ---- framebuffer (epd_fb.c) ----
 * Linha a linha, 25 bytes por linha, MSB = pixel da esquerda, bit 1 = PRETO:
 * o formato que o esp_lcd_ssd1681 espera com invert_color = false. */
uint8_t *epd_fb(void);
void epd_fb_clear(void);                      /* tudo branco */
void epd_fb_pixel(int x, int y, bool black);
bool epd_fb_get(int x, int y);
void epd_fb_rect(int x, int y, int w, int h, bool black);
void epd_fb_hline(int x, int y, int w, bool black);
void epd_fb_invert_rect(int x, int y, int w, int h);

/* ---- vidro (epd_bsp.c) ---- */

/* SPI + painel + primeira limpeza (refresh total). false se o painel não
 * respondeu (BUSY preso). */
bool epd_bsp_init(void);

/* Política de refresh: rápido (LUT de refresh rápido, sem o pisca) é o
 * caminho normal; total (LUT de fábrica, pisca, limpa o ghosting) no boot, a
 * pedido, e a cada EPD_FAST_BUDGET rápidos. Quem chama só pede quando a tela
 * mudou de verdade. O número é chute até medir no vidro. */
#define EPD_FAST_BUDGET 8
/* false = o BUSY não caiu: o vidro não aceitou a imagem, e quem chamou tem de
 * pedir outro refresh em vez de achar que a tela está atualizada */
bool epd_bsp_flush(bool full);

int epd_bsp_flush_count(void);
int epd_bsp_last_flush_ms(void);

/* Põe o controlador em deep sleep (a imagem fica no vidro sem consumo). */
void epd_bsp_sleep(void);

#ifdef __cplusplus
}
#endif
