/* Trilhas de energia e o barramento I2C compartilhado da placa.
 *
 * As três trilhas (VBAT, EPD, áudio) nascem desligadas pelos pull-ups. O VBAT
 * é a trava de alimentação: na bateria, a placa só continua ligada depois que
 * o firmware sobe o GPIO17 — por isso board_power_on() é a primeira coisa do
 * app_main.
 *
 * I2C: um único driver, instalado aqui, atende RTC (0x51), SHTC3 (0x70) e o
 * ES8311 (0x18). Tem de ser o driver legado (driver/i2c.h): no IDF 5.2 o
 * esp_codec_dev usa o legado, e o IDF aborta no boot se o legado e o novo
 * (i2c_master) estiverem no mesmo binário (check_i2c_driver_conflict).
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* VBAT -> EPD, na ordem do exemplo oficial (07_BATT_PWR_Test). */
void board_power_on(void);

/* Liga/desliga a trilha do codec + amplificador (ativa em nível baixo). */
void board_audio_power(bool on);

/* Instala o driver I2C legado na porta da placa; idempotente. */
esp_err_t board_i2c_init(void);

/* Desliga tudo; na bateria a placa apaga (o painel guarda a imagem). */
void board_power_off(void);

#ifdef __cplusplus
}
#endif
