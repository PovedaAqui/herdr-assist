/* Teste de host do pager (epaper154): tela, foco e botões — sem placa.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc -Iscripts/hoststub -Iepaper154/src \
 *      -o /tmp/put epaper154/scripts/pager_ui_test.c epaper154/src/pager_ui.c \
 *      epaper154/src/epd_fb.c epaper154/src/buttons_machine.c \
 *      epaper154/src/portal_form.c epaper154/src/battery_level.c && /tmp/put [dir]
 *
 * Com [dir], grava cada tela como PBM (200x200, 1 bit) — é de onde saem as
 * imagens do README. O herdr_model aqui é um dublê: só os quatro getters que
 * a UI usa.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "battery.h"
#include "buttons.h"
#include "epd_bsp.h"
#include "herdr_model.h"
#include "pager_ui.h"
#include "portal.h"

/* ---------------------------------------------------------------- dublê */
static herdr_agent_t g_agents[HERDR_MAX_AGENTS_TOTAL];
static int g_n;
static uint32_t g_gen;
static herdr_conn_state_t g_conn = HERDR_CONN_ONLINE;

int herdr_model_get_agents(herdr_agent_t *out, int max)
{
    int n = g_n < max ? g_n : max;
    memcpy(out, g_agents, sizeof(herdr_agent_t) * n);
    return n;
}
herdr_conn_state_t herdr_model_get_conn(int host) { return host == 0 ? g_conn : HERDR_CONN_OFFLINE; }
uint32_t herdr_model_generation(void) { return g_gen; }

static void add(const char *pane, const char *agent, const char *project, const char *status, uint32_t since)
{
    herdr_agent_t *a = &g_agents[g_n++];
    memset(a, 0, sizeof(*a));
    snprintf(a->pane_id, sizeof(a->pane_id), "%s", pane);
    snprintf(a->agent, sizeof(a->agent), "%s", agent);
    snprintf(a->project, sizeof(a->project), "%s", project);
    snprintf(a->status, sizeof(a->status), "%s", status);
    a->since = since;
    g_gen++;
}

/* ---------------------------------------------------------------- checks */
static int g_fail, g_pass;
#define CHECK(cond, msg) do { if (cond) g_pass++; else { g_fail++; printf("FAIL: %s\n", msg); } } while (0)

static const char *g_dir;
static void dump(const char *name)
{
    if (!g_dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.pbm", g_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P4\n%d %d\n", EPD_W, EPD_H);
    fwrite(epd_fb(), 1, EPD_FB_BYTES, f);   /* PBM: 1 = preto, MSB primeiro — o mesmo formato */
    fclose(f);
}

static int ink(int x, int y, int w, int h)
{
    int n = 0;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) n += epd_fb_get(i, j);
    return n;
}

/* ---------------------------------------------------------------- botões */
static btn_id_t ev_id[16];
static btn_kind_t ev_kind[16];
static int ev_n;
static void on_btn(btn_id_t id, btn_kind_t kind) { ev_id[ev_n] = id; ev_kind[ev_n] = kind; ev_n++; }

/* segura os botões indicados de t0 a t1 (ms), amostrando a cada 10 ms */
static void run(btn_machine_t *m, bool pwr, bool boot, int64_t *t, int ms)
{
    bool p[2] = { pwr, boot };
    for (int i = 0; i < ms; i += 10) { buttons_machine_step(m, p, *t, on_btn); *t += 10; }
}

static void test_buttons(void)
{
    btn_machine_t m;
    int64_t t = 1000;

    buttons_machine_reset(&m, t); ev_n = 0;
    run(&m, true, false, &t, 200); run(&m, false, false, &t, 100);
    CHECK(ev_n == 1 && ev_id[0] == BTN_PWR && ev_kind[0] == BTN_SHORT, "PWR curto = SHORT do PWR (não do BOOT)");

    buttons_machine_reset(&m, t); ev_n = 0;
    run(&m, false, true, &t, 1200); run(&m, false, false, &t, 100);
    CHECK(ev_n == 1 && ev_id[0] == BTN_BOOT && ev_kind[0] == BTN_LONG, "BOOT 1,2 s = LONG, uma vez só");

    buttons_machine_reset(&m, t); ev_n = 0;
    run(&m, true, false, &t, 3500); run(&m, false, false, &t, 100);
    CHECK(ev_n == 1 && ev_kind[0] == BTN_HOLD, "PWR 3,5 s = HOLD, e a soltura não gera SHORT/LONG");

    buttons_machine_reset(&m, t); ev_n = 0;
    run(&m, true, false, &t, 100); run(&m, true, true, &t, 3200); run(&m, false, false, &t, 100);
    CHECK(ev_n == 1 && ev_id[0] == BTN_BOTH && ev_kind[0] == BTN_CHORD, "PWR+BOOT 3 s = CHORD, sem eventos individuais");

    buttons_machine_reset(&m, t); ev_n = 0;
    run(&m, true, true, &t, 500); run(&m, false, false, &t, 100);
    CHECK(ev_n == 0, "acorde curto não gera nada");

    buttons_machine_reset(&m, t); ev_n = 0;
    run(&m, true, false, &t, 10); run(&m, false, false, &t, 100);
    CHECK(ev_n == 0, "pulso de 10 ms (ruído) é filtrado pelo debounce");
}

/* ---------------------------------------------------------------- tela */
static void test_ui(void)
{
    time_t now = 1760000000;   /* 2025-10-09 */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    pager_ui_set_flags(true, false, false);
    pager_ui_set_battery(87);
    pager_ui_set_status("online: workstation");

    /* boot: sem agentes */
    g_n = 0;
    CHECK(pager_ui_render(now), "primeira renderização sempre muda");
    dump("01-no-agents");
    CHECK(!pager_ui_render(now), "nada mudou: render devolve false (e-paper fica quieto)");

    /* dois trabalhando, um pronto */
    add("p1", "claude", "web-app", "working", (uint32_t)now - 754);
    add("p2", "codex", "api", "working", (uint32_t)now - 7300);
    add("p3", "claude", "lib", "done", 0);
    CHECK(pager_ui_render(now), "agentes chegaram: muda");
    CHECK(pager_ui_waiting() == 0, "ninguém esperando");
    char tgt[32];
    CHECK(!pager_ui_answer_target(NULL, tgt, sizeof(tgt), NULL, 0),
          "sem ninguém esperando, o BOOT não tem alvo (nada de Enter às cegas)");
    CHECK(ink(0, 21, 200, 57) < 2000, "sem espera, a banda de alerta não fica preta");
    dump("02-all-clear");

    /* um bloqueia (vai para o topo) */
    strcpy(g_agents[1].status, "blocked"); g_agents[1].since = 0; g_gen++;
    CHECK(pager_ui_render(now), "bloqueio muda a tela");
    CHECK(pager_ui_waiting() == 1, "um esperando");
    char pane[32], proj[32]; int host = -1;
    CHECK(pager_ui_focused(&host, pane, sizeof(pane), proj, sizeof(proj)) && !strcmp(pane, "p2"),
          "sem foco prévio, o foco vai para o primeiro esperando");
    CHECK(pager_ui_answer_target(NULL, tgt, sizeof(tgt), NULL, 0) && !strcmp(tgt, "p2"),
          "BOOT mira quem está esperando");
    /* bancada: com um só esperando, o PWR reescolhia o mesmo pane e o botão
       parecia morto na tela de espera */
    pager_ui_focus_next();
    pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0);
    CHECK(strcmp(pane, "p2") != 0, "com um só esperando, o PWR anda");
    pager_ui_focus_next();
    pager_ui_focus_next();                       /* 3 linhas: fecha o ciclo */
    pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0);
    CHECK(!strcmp(pane, "p2"), "o ciclo do PWR volta ao mesmo pane");
    CHECK(ink(0, 21, 200, 57) > 5000, "com espera, a banda de alerta fica invertida (preta)");
    dump("03-one-waiting");

    /* mais um bloqueia; o foco fica no p2 mesmo se a ordem mudar */
    strcpy(g_agents[0].status, "blocked"); g_agents[0].since = 0; g_gen++;
    pager_ui_render(now);
    CHECK(pager_ui_waiting() == 2, "dois esperando");
    pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0);
    CHECK(!strcmp(pane, "p2"), "o foco segue o pane, não a posição na lista");
    pager_ui_focus_next();
    pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0);
    CHECK(!strcmp(pane, "p3"), "PWR anda uma posição: quem espera vem primeiro, depois o resto");
    pager_ui_focus_next();
    pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0);
    CHECK(!strcmp(pane, "p1"), "do fim da lista volta ao topo, onde ficam os que esperam");
    pager_ui_focus_next();
    pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0);
    CHECK(!strcmp(pane, "p2"), "e segue em ciclo");
    /* com o foco num agente ocupado, o BOOT continua mirando quem espera */
    pager_ui_focus_next();
    pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0);
    CHECK(!strcmp(pane, "p3"), "o foco anda até um agente que trabalha");
    CHECK(pager_ui_answer_target(NULL, tgt, sizeof(tgt), NULL, 0) && !strcmp(tgt, "p1"),
          "BOOT mira quem espera, não o foco");
    pager_ui_focus_next();          /* o foco volta para quem espera: é o que a
                                       imagem do README mostra */
    pager_ui_render(now);
    dump("04-two-waiting");

    /* lista cheia, nomes longos e UTF-8 */
    add("p4", "gemini", "a-very-long-project-name", "working", (uint32_t)now - 30);
    add("p5", "claude", "analytics", "idle", 0);
    add("p6", "claude", "infra", "unknown", 0);
    add("p7", "codex", "web", "working", (uint32_t)now - 120);
    add("p8", "claude", "docs", "idle", 0);
    pager_ui_set_flags(true, true, true);
    pager_ui_set_battery(12);
    pager_ui_render(now);
    CHECK(ink(150, 12, 50, 8) > 150, "bateria baixa: o indicador fica invertido");
    CHECK(ink(0, 200 - 1, 200, 1) == 0, "nada desenhado na última linha (margem)");
    dump("05-full-list-muted-update");

    /* três esperando ao mesmo tempo, com mais sessões que linhas */
    strcpy(g_agents[3].status, "blocked"); g_agents[3].since = 0; g_gen++;   /* p4 */
    pager_ui_render(now);
    CHECK(pager_ui_waiting() == 3, "três esperando ao mesmo tempo");
    CHECK(ink(0, 21, 200, 57) > 5000, "a banda de alerta continua invertida com três esperando");
    CHECK(pager_ui_answer_target(NULL, tgt, sizeof(tgt), NULL, 0) && !strcmp(tgt, "p1"),
          "com três esperando, o alvo do BOOT é o primeiro da lista");

    /* o PWR percorre a lista inteira e o foco tem de continuar visível */
    for (int i = 0; i < g_n; i++) {
        pager_ui_focus_next();
        pager_ui_render(now);
        int inv = 0;
        for (int r = 0; r < 5; r++) {
            if (ink(0, 80 + r * 16, 200, 15) > 2000) inv++;
        }
        CHECK(inv == 1, "com mais sessões que linhas, exatamente uma linha focada aparece");
    }

    /* ponte fora */
    g_n = 0; g_gen++; g_conn = HERDR_CONN_OFFLINE;
    pager_ui_set_flags(true, false, false);
    pager_ui_set_battery(-1);
    pager_ui_set_status("bridge unreachable");
    pager_ui_render(0);
    dump("06-no-bridge");

    /* a ponte cai, mas o último retrato continua na lista: a banda não pode
       dizer ALL CLEAR nem prometer BOOT — nada daquilo é ao vivo */
    g_n = 3; g_gen++;
    pager_ui_render(now);
    dump("06b-offline-with-rows");
    g_n = 0; g_gen++;
    g_conn = HERDR_CONN_ONLINE;

    pager_ui_set_screen(UI_SETUP);
    pager_ui_set_setup("herdr-3fa2b1", "48213907", "192.168.4.1");
    CHECK(pager_ui_render(now), "tela de setup muda");
    dump("07-setup");

    pager_ui_set_screen(UI_PAIRING);
    pager_ui_set_pairing("3fa2b1", 142, true);
    pager_ui_render(now);
    dump("08-pairing");

    pager_ui_set_screen(UI_MESSAGE);
    pager_ui_set_message("HERDR PAGER", "herdr-assist v0.13.0 (epaper154)", "starting Wi-Fi...");
    pager_ui_render(now);
    dump("09-boot");

    /* descanso: ponte fora do ar, avatar até alguém apertar um botão */
    pager_ui_set_screen(UI_SLEEP);
    CHECK(pager_ui_render(now), "tela de descanso muda");
    dump("10-sleep");
}

/* ---------------------------------------------------------------- portal */
static void test_portal_form(void)
{
    char out[65];
    const char *body = "ssid=Casa+da+M%C3%A3e&pass=p%40ss%26w0rd%3D1&tz=3";
    CHECK(portal_form_get(body, "ssid", out, sizeof(out)) && !strcmp(out, "Casa da M\xc3\xa3" "e"),
          "form: + vira espaço e %XX decodifica UTF-8");
    CHECK(portal_form_get(body, "pass", out, sizeof(out)) && !strcmp(out, "p@ss&w0rd=1"),
          "form: & e = codificados ficam dentro do valor");
    CHECK(portal_form_get(body, "tz", out, sizeof(out)) && !strcmp(out, "3"), "form: último campo");
    CHECK(!portal_form_get(body, "ss", out, sizeof(out)), "form: prefixo de chave não casa");
    char small[4];
    CHECK(!portal_form_get(body, "ssid", small, sizeof(small)), "form: valor grande demais é recusado, não truncado");
    CHECK(portal_form_get("ssid=&pass=", "ssid", out, sizeof(out)) && out[0] == '\0', "form: valor vazio");
}

/* ---------------------------------------------------------------- bateria */
static void test_battery(void)
{
    CHECK(battery_percent_from_mv(0) == -1, "bateria: 0 mV = sem célula (indicador some)");
    CHECK(battery_percent_from_mv(4200) == 100 && battery_percent_from_mv(4120) == 100, "bateria: cheia = 100");
    CHECK(battery_percent_from_mv(3000) == 0 && battery_percent_from_mv(2900) == 0, "bateria: 3,0 V = 0");
    CHECK(battery_percent_from_mv(3800) == 55, "bateria: ponto da curva (3,8 V = 55)");
    int prev = 101, mono = 1;
    for (int mv = 4200; mv >= 3000; mv -= 10) {
        int p = battery_percent_from_mv(mv);
        if (p > prev) mono = 0;
        prev = p;
    }
    CHECK(mono, "bateria: a curva nunca sobe quando a tensão cai");
}

/* uma sessão só: o PWR seleciona a única linha e depois não tem para onde
 * andar. A tela tem de ficar parada — o rodapé é que troca "next" por "focus" */
static void test_single_row(void)
{
    time_t now = 1760000000;
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    pager_ui_set_flags(true, false, false);
    pager_ui_set_battery(87);
    pager_ui_set_status("online: luis-pc");   /* test_ui deixa outro status */
    g_conn = HERDR_CONN_ONLINE;
    pager_ui_set_screen(UI_MAIN);        /* test_ui termina em outra tela */

    g_n = 0;
    add("p1", "omp", "solo", "working", (uint32_t)now - 60);
    CHECK(pager_ui_render(now), "com uma linha, a primeira renderização muda");

    char pane[32];
    CHECK(!pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0),
          "uma linha trabalhando e sem espera: ainda não há seleção");
    pager_ui_focus_next();
    CHECK(pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0) && !strcmp(pane, "p1"),
          "uma linha: o PWR seleciona a única sessão");
    CHECK(pager_ui_render(now), "e a linha inverte: a tela muda");
    pager_ui_focus_next();
    CHECK(pager_ui_focused(NULL, pane, sizeof(pane), NULL, 0) && !strcmp(pane, "p1"),
          "uma linha: de novo, o foco continua nela (não há para onde andar)");
    CHECK(!pager_ui_render(now), "e a tela não redesenha nada");
}

int main(int argc, char **argv)
{
    g_dir = argc > 1 ? argv[1] : NULL;
    test_buttons();
    test_portal_form();
    test_battery();
    test_ui();
    test_single_row();
    printf("%d checks, %d falhas\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
