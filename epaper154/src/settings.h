/* Preferências do pager que não cabem no panel_cfg compartilhado: fuso
 * horário e sino mudo. NVS própria (namespace "pager"), para não mexer no
 * struct que o painel e o Cardputer também gravam. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fuso no formato POSIX (ex.: "CET-1CEST,M3.5.0,M10.5.0/3"). false = nunca
 * escolhido (o net.c compartilhado cai no padrão dele). */
bool settings_get_tz(char *out, size_t len);
void settings_set_tz(const char *tz);

bool settings_get_muted(void);
void settings_set_muted(bool muted);

#ifdef __cplusplus
}
#endif
