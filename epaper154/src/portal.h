/* Configuração do Wi-Fi sem teclado: ponto de acesso + página web.
 *
 * O painel tem touch e o Cardputer tem teclado; o pager tem dois botões. Então
 * o pager abre uma rede própria (WPA2, senha de 8 dígitos sorteada e mostrada
 * na tela), e um celular entra nela e escolhe a rede da casa e o fuso numa
 * página em http://192.168.4.1. Ao salvar, grava no panel_cfg (o mesmo struct
 * dos outros devices) e reinicia. O token da ponte NÃO passa por aqui: ele
 * vem depois, pelo pareamento com o host (pairing.c, compartilhado).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORTAL_URL "192.168.4.1"

/* Requer net_wifi_init() antes. Devolve o SSID e a senha da rede aberta. */
esp_err_t portal_start(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len);

/* Formulário application/x-www-form-urlencoded (portal_form.c, C puro —
 * públicos só para o teste de host). portal_form_get devolve false se a
 * chave falta ou se o valor não cabe em out (nunca trunca uma senha). */
void portal_url_decode(char *s);
bool portal_form_get(const char *body, const char *key, char *out, size_t len);

#ifdef __cplusplus
}
#endif
