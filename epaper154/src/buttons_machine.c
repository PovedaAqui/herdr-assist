/* Classificação dos botões — C puro, sem GPIO (testada no host). */

#include "buttons.h"

#include <string.h>

void buttons_machine_reset(btn_machine_t *m, int64_t now_ms)
{
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < 2; i++) m->since[i] = m->raw_since[i] = now_ms;
}

void buttons_machine_step(btn_machine_t *m, const bool pressed[2], int64_t now_ms, btn_cb_t cb)
{
    for (int i = 0; i < 2; i++) {
        if (pressed[i] != m->raw[i]) {
            m->raw[i] = pressed[i];
            m->raw_since[i] = now_ms;
        }
        if (m->raw[i] == m->down[i] || now_ms - m->raw_since[i] < BTN_DEBOUNCE_MS) continue;

        /* mudança estável */
        if (m->raw[i]) {
            m->down[i] = true;
            m->since[i] = m->raw_since[i];
            m->consumed[i] = false;
        } else {
            int64_t held = m->raw_since[i] - m->since[i];
            m->down[i] = false;
            m->since[i] = m->raw_since[i];
            if (!m->consumed[i] && cb)
                cb((btn_id_t)i, held >= BTN_LONG_MS ? BTN_LONG : BTN_SHORT);
            m->consumed[i] = false;
        }
    }

    if (m->down[0] && m->down[1]) {
        if (!m->chord_armed) {
            m->chord_armed = true;
            m->chord_fired = false;
            m->chord_since = m->since[0] > m->since[1] ? m->since[0] : m->since[1];
            m->consumed[0] = m->consumed[1] = true;   /* soltar depois é mudo */
        }
        if (!m->chord_fired && now_ms - m->chord_since >= BTN_HOLD_MS) {
            m->chord_fired = true;
            if (cb) cb(BTN_BOTH, BTN_CHORD);
        }
        return;
    }
    if (!m->down[0] && !m->down[1]) m->chord_armed = false;
    if (m->chord_armed) return;   /* um ainda apertado depois do acorde */

    for (int i = 0; i < 2; i++) {
        if (m->down[i] && !m->consumed[i] && now_ms - m->since[i] >= BTN_HOLD_MS) {
            m->consumed[i] = true;
            if (cb) cb((btn_id_t)i, BTN_HOLD);
        }
    }
}
