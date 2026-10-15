/* A tela do pager: 200x200 mono, C puro (sem LVGL, sem IDF).
 *
 * Quatro faixas na tela principal — topo (hora, mudo, link), banda de alerta
 * (quem espera), lista de sessões (esperando primeiro) e rodapé (legenda dos
 * botões + estado) — e três telas de serviço: configuração do Wi-Fi,
 * pareamento com o host e mensagem.
 *
 * pager_ui_render() sempre redesenha no framebuffer e devolve true só se o
 * quadro ficou diferente do último que foi para o vidro: quem chama faz o
 * flush só nesse caso. É o que deixa o e-paper quieto quando nada muda.
 *
 * Testada no host por scripts/pager_ui_test.c (gera as telas como imagem).
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_MAIN = 0,
    UI_SETUP,      /* ponto de acesso do Wi-Fi aberto */
    UI_PAIRING,    /* janela de pareamento com o host */
    UI_MESSAGE,    /* título + duas linhas (boot, erro, reinício) */
} ui_screen_t;

void pager_ui_set_screen(ui_screen_t screen);
ui_screen_t pager_ui_screen(void);

void pager_ui_set_setup(const char *ap_ssid, const char *ap_pass, const char *url);
void pager_ui_set_pairing(const char *device_id, int secs_left, bool wifi_up);
void pager_ui_set_message(const char *title, const char *line1, const char *line2);

/* Rodapé: texto curto de estado ("wifi...", "no bridge"); NULL limpa. */
void pager_ui_set_status(const char *status);
void pager_ui_set_flags(bool wifi_up, bool muted, bool update_available);
/* Carga da bateria em %, ou -1 sem bateria (o indicador some). */
void pager_ui_set_battery(int pct);

/* now = 0 quando o relógio ainda não é confiável (mostra --:--). */
bool pager_ui_render(time_t now);

/* Foco: a sessão que o PWR seleciona e o BOOT responde. A lista é a da última
 * renderização (a mesma ordem que está no vidro). */
void pager_ui_focus_next(void);
bool pager_ui_focused(int *host, char *pane_id, int pane_len, char *project, int project_len);

/* Quantos agentes estão em `blocked` na última renderização. */
int pager_ui_waiting(void);

#ifdef __cplusplus
}
#endif
