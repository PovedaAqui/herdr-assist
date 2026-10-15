/* PCF85063A (RTC) no I2C da placa: guarda a hora entre um boot e outro.
 *
 * Fluxo: no boot, rtc_clock_restore() copia o RTC para o relógio do sistema
 * (se o RTC for confiável); quando o SNTP sincroniza, rtc_clock_save() grava
 * a hora certa de volta. A UI só lê o relógio do sistema (time/localtime). */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Usa o I2C da placa (board_i2c_init) e solta o bit STOP se estiver preso. */
bool rtc_clock_init(void);

/* RTC -> relógio do sistema (UTC). false se o RTC perdeu a hora (bit OS). */
bool rtc_clock_restore(void);

/* Relógio do sistema (UTC) -> RTC. */
bool rtc_clock_save(void);

/* true se o relógio do sistema parece válido (ano >= 2024). */
bool rtc_clock_system_valid(void);

#ifdef __cplusplus
}
#endif
