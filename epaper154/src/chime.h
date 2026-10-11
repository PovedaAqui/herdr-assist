/* O sino: ES8311 + I2S. Toca quando um agente ENTRA em `blocked` — nunca em
 * estado parado (plano §5, linha 6). */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHIME_BLOCKED,   /* um agente bloqueou: dois tons subindo */
    CHIME_MORE,      /* o número de esperando subiu: três tons */
    CHIME_TEST,      /* um tom, toca mesmo no mudo (confirma o desmute) */
} chime_kind_t;

/* Liga a trilha do áudio, sobe I2S (MCLK) e só então o ES8311. */
bool chime_init(void);

/* Não bloqueia: enfileira para a task do sino. */
void chime_play(chime_kind_t kind);
void chime_set_muted(bool muted);
bool chime_is_muted(void);

#ifdef __cplusplus
}
#endif
