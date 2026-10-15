/* Os dois botões: PWR (GPIO18) e BOOT (GPIO0), confirmados no user_config.h
 * do exemplo oficial da Waveshare (ver board_pins.h).
 *
 * Gramática — a única entrada que a placa oferece:
 *   SHORT  solto antes de 0,7 s
 *   LONG   solto entre 0,7 s e 3 s
 *   HOLD   segurado 3 s (dispara ainda segurando; a soltura não gera nada)
 *   CHORD  os dois segurados juntos por 3 s (id = BTN_BOTH); enquanto os
 *          dois estiverem apertados, nenhum evento individual sai
 *
 * Classificação em C puro (buttons_classify), para o teste de host. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_PWR = 0,
    BTN_BOOT = 1,
    BTN_BOTH = 2,
} btn_id_t;

typedef enum {
    BTN_SHORT,
    BTN_LONG,
    BTN_HOLD,
    BTN_CHORD,
} btn_kind_t;

#define BTN_DEBOUNCE_MS  30
#define BTN_LONG_MS     700
#define BTN_HOLD_MS    3000

typedef void (*btn_cb_t)(btn_id_t id, btn_kind_t kind);

/* Sobe os GPIOs e a task de varredura (10 ms). */
bool buttons_init(btn_cb_t cb);

/* ---- máquina de estados, sem hardware ---- */
typedef struct {
    bool     down[2];        /* nível estável (true = apertado) */
    int64_t  since[2];       /* ms em que o estado estável começou */
    bool     raw[2];
    int64_t  raw_since[2];
    bool     consumed[2];    /* HOLD/CHORD já saiu: a soltura é muda */
    bool     chord_armed;    /* os dois estão apertados */
    bool     chord_fired;
    int64_t  chord_since;
} btn_machine_t;

void buttons_machine_reset(btn_machine_t *m, int64_t now_ms);
/* Alimenta um amostra (pressed[i] = nível cru); chama cb para cada evento. */
void buttons_machine_step(btn_machine_t *m, const bool pressed[2], int64_t now_ms, btn_cb_t cb);

#ifdef __cplusplus
}
#endif
