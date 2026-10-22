/* Bateria da placa: LiPo 3,7 V no conector MX1.25, lida no ADC1 canal 3
 * (GPIO4) atrás de um divisor 1:2 — o mesmo dos exemplos oficiais
 * (01_ADC_Test e 11_FactoryProgram/port_adc.cpp, V1 e V2 iguais).
 *
 * battery_percent_from_mv() é C puro (testada no host); o resto lê o ADC. */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Abaixo disto não há célula: só o USB (ou leitura sem sentido). */
#define BATTERY_ABSENT_MV   2500
/* Desliga com aviso abaixo disto (três leituras seguidas): protege a célula
 * e deixa o motivo no vidro. */
#define BATTERY_CUTOFF_MV   3300

bool battery_init(void);
/* Média de 8 leituras, em mV na célula (já desfeito o divisor); -1 se falhar. */
int  battery_read_mv(void);
/* Curva aproximada de LiPo sob carga leve; -1 = sem bateria. */
int  battery_percent_from_mv(int mv);

#ifdef __cplusplus
}
#endif
