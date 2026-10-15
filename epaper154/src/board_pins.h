/* Pinos da Waveshare ESP32-S3-ePaper-1.54.
 *
 * Fonte: user_config.h dos exemplos ESP-IDF oficiais da própria Waveshare
 * (repositório waveshareteam/ESP32-S3-ePaper-1.54, commit aaf395c,
 * 02_Example/ESP-IDF/V{1,2}/07_BATT_PWR_Test/main/user_config.h). Os valores
 * são idênticos em V1 e V2 — este header só existe para ter tudo num lugar em
 * vez de espalhar números pelos fontes.
 */

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ---- painel (SSD1681, 200x200 mono) ---- */
#define BOARD_EPD_SPI_HOST   SPI2_HOST
#define BOARD_EPD_DC_PIN     GPIO_NUM_10
#define BOARD_EPD_CS_PIN     GPIO_NUM_11
#define BOARD_EPD_SCK_PIN    GPIO_NUM_12
#define BOARD_EPD_MOSI_PIN   GPIO_NUM_13
#define BOARD_EPD_RST_PIN    GPIO_NUM_9
#define BOARD_EPD_BUSY_PIN   GPIO_NUM_8
#define BOARD_EPD_FB_BYTES   5000   /* 200*200/8 — o buffer_len do exemplo */

/* ---- trilhas de energia (o exemplo liga todas antes de falar com o painel) ----
 * EPD e Audio são ativos em nível BAIXO; VBAT é ativo em nível ALTO. Os três
 * nascem com pull-up, então nada responde antes de serem ligados. */
#define BOARD_EPD_PWR_PIN    GPIO_NUM_6    /* low = ligado */
#define BOARD_AUDIO_PWR_PIN  GPIO_NUM_42   /* low = ligado */
#define BOARD_VBAT_PWR_PIN   GPIO_NUM_17   /* high = ligado */

/* ---- botões ---- */
#define BOARD_BOOT_BTN_PIN   GPIO_NUM_0
#define BOARD_PWR_BTN_PIN    GPIO_NUM_18

/* ---- I2C compartilhado: RTC, SHTC3, ES8311 ---- */
#define BOARD_I2C_PORT       0        /* I2C_NUM_0 */
#define BOARD_I2C_SDA_PIN    GPIO_NUM_47
#define BOARD_I2C_SCL_PIN    GPIO_NUM_48
#define BOARD_I2C_FREQ_HZ    100000   /* o mesmo do codec_board do vendor */
#define BOARD_RTC_ADDR       0x51   /* PCF85063A */
#define BOARD_SHTC3_ADDR     0x70
#define BOARD_ES8311_ADDR    0x18

/* ---- I2S (codec ES8311 + PA) ----
 * Valores do board_cfg.txt do próprio exemplo (entrada `Board: S3_ePaper_1_54`),
 * que é a fonte autoritativa: o codec_board/audio_bsp leem essa entrada.
 * Atenção ao sentido: dout=45 é o I2S do ESP32 PARA o codec, din=16 é o
 * retorno (microfone) — a tabela do revendedor trazia os dois trocados. */
#define BOARD_I2S_MCLK_PIN   GPIO_NUM_14
#define BOARD_I2S_BCLK_PIN   GPIO_NUM_15
#define BOARD_I2S_DOUT_PIN   GPIO_NUM_45   /* ESP32 -> ES8311 (sdout) */
#define BOARD_I2S_LRCK_PIN   GPIO_NUM_38
#define BOARD_I2S_DIN_PIN    GPIO_NUM_16   /* ES8311 -> ESP32 (mic) */
#define BOARD_I2S_PA_CTRL    GPIO_NUM_46   /* pa: 46 no board_cfg */
