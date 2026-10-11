/* Curva de carga da LiPo — C puro, sem ADC (testada no host).
 *
 * O exemplo oficial usa uma reta de 3,00 V (0 %) a 4,12 V (100 %); a LiPo é
 * plana no meio, então a reta mostra 50 % quando sobra bem mais. Os pontos
 * abaixo são uma curva genérica de LiPo 1S sob carga leve, com as mesmas
 * pontas do exemplo. PENDENTE DE BANCADA: conferir contra uma descarga real. */

#include "battery.h"

static const struct { int mv, pct; } CURVE[] = {
    { 4120, 100 }, { 4000, 85 }, { 3900, 72 }, { 3800, 55 }, { 3700, 35 },
    { 3600, 18 }, { 3500, 8 }, { 3300, 2 }, { 3000, 0 },
};

int battery_percent_from_mv(int mv)
{
    if (mv < BATTERY_ABSENT_MV) return -1;
    if (mv >= CURVE[0].mv) return 100;
    int n = sizeof(CURVE) / sizeof(CURVE[0]);
    for (int i = 1; i < n; i++) {
        if (mv >= CURVE[i].mv) {
            int dm = CURVE[i - 1].mv - CURVE[i].mv;
            int dp = CURVE[i - 1].pct - CURVE[i].pct;
            return CURVE[i].pct + (mv - CURVE[i].mv) * dp / dm;
        }
    }
    return 0;
}
