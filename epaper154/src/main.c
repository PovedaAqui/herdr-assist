/* herdr-assist no Waveshare ESP32-S3-ePaper-1.54 — o pager.
 *
 * Um device que só faz uma coisa: mostra qual agente precisa de você e deixa
 * responder sem sair da mesa. Três estados, nesta ordem:
 *   1. sem Wi-Fi configurado -> ponto de acesso + página (portal.c);
 *   2. com Wi-Fi e sem host  -> janela de pareamento (pairing.c, compartilhado:
 *      o host Herdr entrega nome, endereço, porta e token);
 *   3. pareado               -> tela principal, sino e os dois botões.
 *
 * Toda a lógica de tela roda numa task só (ui_task); o callback dos botões só
 * enfileira, então pager_ui nunca é tocado por duas tasks.
 *
 * Botões (buttons.h):
 *   PWR curto   foca a próxima sessão (esperando primeiro) e manda `focus`
 *   PWR longo   abre a janela de pareamento (novo host / reparear)
 *   PWR 3 s     desliga (deep sleep; o PWR acorda)
 *   BOOT curto  manda Enter à sessão em foco (allowlist aplicada na ponte)
 *   BOOT longo  sino mudo / ligado
 *   BOOT 3 s    instala a atualização de firmware, se houver
 *   PWR+BOOT 3s configuração do Wi-Fi (ou cancela, se já estiver nela)
 *
 * Bateria: o gerenciador de energia do IDF entra em light sleep sozinho
 * entre os eventos (Wi-Fi em modem sleep segura a conexão com a ponte, então
 * o sino continua chegando na hora). Com um PC ligado na USB o light sleep
 * fica suspenso (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION) para o log funcionar;
 * num carregador de parede ele vale normalmente.
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "battery.h"
#include "board.h"
#include "board_pins.h"
#include "buttons.h"
#include "chime.h"
#include "epd_bsp.h"
#include "fw_update.h"
#include "herdr_conn.h"
#include "herdr_model.h"
#include "net.h"
#include "pager_ui.h"
#include "pairing.h"
#include "panel_cfg.h"
#include "portal.h"
#include "rtc_clock.h"
#include "settings.h"

static const char *TAG = "pager";

/* último motivo de queda do STA: é o que separa "senha errada" de "rede fora
   do ar" na tela de erro — sem ele o pager ficava mudo e offline para sempre */
static volatile uint8_t s_wifi_reason;
/* senha errada aparece como AUTH_FAIL, mas a re-tentativa seguinte pode cair
   em NO_AP_FOUND e sobrescrever o motivo; isto guarda o que já foi visto */
static volatile bool s_wifi_auth_failed;

typedef struct { btn_id_t id; btn_kind_t kind; } btn_evt_t;

static QueueHandle_t s_btn_q;
static volatile bool s_sntp_synced;
static bool s_conn_started;
static char s_status[40];
/* sobrevive ao esp_restart (não ao corte de energia): "suba direto no setup"
 * sem apagar o Wi-Fi salvo — cancelar o setup volta à rede de antes */
#define SETUP_MAGIC 0x5e7a95e7u
/* Ponte fora do ar por este tempo e o pager descansa no avatar: para de
   tentar (bateria) e só volta a procurar quando alguém aperta um botão. */
#define SLEEP_AFTER_US  (120 * 1000000LL)
RTC_NOINIT_ATTR static uint32_t s_boot_into_setup;

/* ------------------------------------------------------------ utilidades */
static bool has_host(void)
{
    const panel_cfg_t *cfg = panel_cfg_get();
    for (int i = 0; i < CFG_MAX_HOSTS; i++)
        if (cfg->hosts[i].enabled && !panel_host_is_free(&cfg->hosts[i])) return true;
    return false;
}

static void show_now(bool full)
{
    time_t now = rtc_clock_system_valid() ? time(NULL) : 0;
    if (pager_ui_render(now) || full) epd_bsp_flush(full);
}

static void say(const char *title, const char *l1, const char *l2)
{
    pager_ui_set_message(title, l1, l2);
    pager_ui_set_screen(UI_MESSAGE);
    show_now(true);
}

static void on_button(btn_id_t id, btn_kind_t kind)
{
    btn_evt_t e = { id, kind };
    xQueueSend(s_btn_q, &e, 0);
}

static void on_sntp(struct timeval *tv) { (void)tv; s_sntp_synced = true; }

/* ------------------------------------------------------------ pareamento */
/* Mesma regra do painel (herdr_ui_settings.c, pair_apply): mesmo nome de host
 * atualiza o slot; senão o mesmo endereço; senão o primeiro livre. */
static bool pair_apply(const panel_host_t *h, bool auto_mode)
{
    panel_cfg_t cfg = *panel_cfg_get();
    panel_host_t inc = *h;
    if (auto_mode) inc.host[0] = '\0';   /* auto: endereço resolvido por broadcast */
    int slot = -1;
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++)
        if (cfg.hosts[i].name[0] && !strcmp(cfg.hosts[i].name, inc.name)) slot = i;
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++)
        if (inc.host[0] && cfg.hosts[i].host[0] && !strcmp(cfg.hosts[i].host, inc.host)) slot = i;
    for (int i = 0; i < CFG_MAX_HOSTS && slot < 0; i++)
        if (panel_host_is_free(&cfg.hosts[i])) slot = i;
    if (slot < 0) return false;
    cfg.hosts[slot] = inc;
    return panel_cfg_save(&cfg) == ESP_OK;
}

static void start_pairing(void)
{
    if (pairing_start() != ESP_OK) {
        ESP_LOGE(TAG, "pairing_start falhou");
        return;
    }
    pager_ui_set_pairing(pairing_device_id(), pairing_seconds_left(), net_wifi_is_up());
    pager_ui_set_screen(UI_PAIRING);
    show_now(true);
}

static void tick_pairing(void)
{
    panel_host_t h;
    bool auto_mode = false;
    switch (pairing_state()) {
    case PAIRING_DONE:
        if (pairing_result(&h, &auto_mode)) {
            pairing_stop();
            if (pair_apply(&h, auto_mode)) {
                say("PAIRED", h.name, "restarting...");
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
            say("PAIR FAILED", "all 4 host slots are taken", "hold PWR+BOOT 3s for Wi-Fi setup");
        }
        break;
    case PAIRING_WAITING:
        pager_ui_set_pairing(pairing_device_id(), pairing_seconds_left(), net_wifi_is_up());
        break;
    default:
        pager_ui_set_pairing(pairing_device_id(), 0, net_wifi_is_up());
        break;
    }
}

/* ------------------------------------------------------------ setup / off */
static void enter_setup(void)
{
    char ssid[33], pass[24];
    if (portal_start(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        say("SETUP FAILED", "could not open the access point", "");
        return;
    }
    pager_ui_set_setup(ssid, pass, PORTAL_URL);
    pager_ui_set_screen(UI_SETUP);
    show_now(true);
}

static void power_off(void)
{
    say("OFF", "press PWR to wake", "");
    epd_bsp_sleep();
    board_power_off();                       /* na bateria, a placa apaga aqui */
    /* no USB a trava não corta: dorme e acorda pelo PWR (GPIO18 é RTC GPIO) */
    esp_sleep_enable_ext0_wakeup(BOARD_PWR_BTN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));          /* soltar o botão antes de dormir */
    esp_deep_sleep_start();
}

/* ------------------------------------------------------------ botões */
static void handle_button(const btn_evt_t *e)
{
    ui_screen_t scr = pager_ui_screen();

    if (e->id == BTN_BOTH && e->kind == BTN_CHORD) {
        if (scr == UI_SETUP) esp_restart();  /* cancela */
        if (s_conn_started) {
            /* sobe direto no setup: um boot sem ponte e com o AP */
            s_boot_into_setup = SETUP_MAGIC;
            say("WI-FI SETUP", "restarting into setup...", "");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        enter_setup();
        return;
    }
    if (scr == UI_SETUP) return;

    if (e->id == BTN_PWR && e->kind == BTN_HOLD) { power_off(); return; }

    if (scr == UI_SLEEP) {
        /* qualquer botão acorda: tenta de novo na hora, sem esperar backoff */
        herdr_conn_wake();
        pager_ui_set_screen(UI_MAIN);
        show_now(true);
        return;
    }

    if (scr == UI_PAIRING) {
        if (e->id == BTN_PWR && (e->kind == BTN_LONG || e->kind == BTN_SHORT) &&
            pairing_state() != PAIRING_WAITING) start_pairing();
        else if (e->id == BTN_BOOT && e->kind == BTN_SHORT && has_host()) {
            pairing_stop();
            pager_ui_set_screen(UI_MAIN);
        }
        return;
    }

    if (scr == UI_MESSAGE) {               /* qualquer botão volta à principal */
        pager_ui_set_screen(UI_MAIN);
        return;
    }

    int host;
    char pane[HERDR_ID_LEN], project[HERDR_NAME_LEN];
    if (e->id == BTN_PWR && e->kind == BTN_SHORT) {
        pager_ui_focus_next();
        if (pager_ui_focused(&host, pane, sizeof(pane), project, sizeof(project))) {
            herdr_conn_focus(host, pane);
            snprintf(s_status, sizeof(s_status), "focus: %s", project);
        }
    } else if (e->id == BTN_PWR && e->kind == BTN_LONG) {
        start_pairing();
    } else if (e->id == BTN_BOOT && e->kind == BTN_SHORT) {
        /* o pager avisa, não conversa: tecla só vai para quem está esperando.
           Mirar o foco mandaria Enter no meio do trabalho de um agente ocupado */
        if (pager_ui_answer_target(&host, pane, sizeof(pane), project, sizeof(project))) {
            static const char *const enter[] = { "Enter" };
            esp_err_t err = herdr_conn_send_keys(host, pane, enter, 1);
            snprintf(s_status, sizeof(s_status), err == ESP_OK ? "Enter -> %s" : "not sent: %s", project);
        } else {
            snprintf(s_status, sizeof(s_status), "nobody waiting");
        }
    } else if (e->id == BTN_BOOT && e->kind == BTN_LONG) {
        bool m = !chime_is_muted();
        chime_set_muted(m);
        settings_set_muted(m);
        if (!m) chime_play(CHIME_TEST);
    } else if (e->id == BTN_BOOT && e->kind == BTN_HOLD) {
        if (fw_update_start()) say("UPDATING", "downloading new firmware", "do not unplug");
    }
}

/* ------------------------------------------------------------ laço da UI */
static void update_status(void)
{
    if (s_status[0]) return;               /* mensagem de ação ainda na tela */
    const panel_cfg_t *cfg = panel_cfg_get();
    static char buf[40];
    if (!net_wifi_is_up()) {
        pager_ui_set_status("wifi: connecting");
        return;
    }
    for (int i = 0; i < CFG_MAX_HOSTS; i++) {
        if (herdr_model_get_conn(i) == HERDR_CONN_ONLINE) {
            snprintf(buf, sizeof(buf), "online: %s", cfg->hosts[i].name[0] ? cfg->hosts[i].name : "host");
            pager_ui_set_status(buf);
            return;
        }
    }
    pager_ui_set_status(has_host() ? "bridge: searching" : "not paired");
}

static void ui_task(void *arg)
{
    (void)arg;
    int prev_waiting = 0;
    bool was_online = false, baseline_pending = true;
    uint32_t last_gen = 0;
    int64_t status_until = 0, next_batt = 0;
    int64_t offline_since = 0;
    int low_batt = 0;
    bool rtc_saved = false;
    ui_screen_t last_screen = pager_ui_screen();

    for (;;) {
        /* dorme até um botão ou 1 s: é aqui que o light sleep acontece */
        btn_evt_t e;
        bool got = xQueueReceive(s_btn_q, &e, pdMS_TO_TICKS(1000)) == pdTRUE;
        while (got) {
            s_status[0] = '\0';
            handle_button(&e);
            if (s_status[0]) {
                pager_ui_set_status(s_status);
                status_until = esp_timer_get_time() + 5 * 1000000LL;
            }
            got = xQueueReceive(s_btn_q, &e, 0) == pdTRUE;
        }
        int64_t t_us = esp_timer_get_time();
        if (status_until && t_us > status_until) { s_status[0] = '\0'; status_until = 0; }

        /* ninguém online por tempo demais: descansa no avatar. O rádio para de
           tentar — é o que segura a bateria quando não há nada para mostrar. */
        if (s_conn_started && pager_ui_screen() == UI_MAIN) {
            bool any = false;
            for (int i = 0; i < CFG_MAX_HOSTS; i++) {
                if (herdr_model_get_conn(i) == HERDR_CONN_ONLINE) { any = true; break; }
            }
            if (any) {
                offline_since = 0;
            } else if (!offline_since) {
                offline_since = t_us;
            } else if (t_us - offline_since > SLEEP_AFTER_US) {
                herdr_conn_suspend(true);
                pager_ui_set_screen(UI_SLEEP);
                show_now(true);
            }
        }

        if (t_us >= next_batt) {
            next_batt = t_us + 30 * 1000000LL;
            int mv = battery_read_mv();
            int pct = battery_percent_from_mv(mv);
            pager_ui_set_battery(pct);
            /* bateria curta: recuar mais entre tentativas de reconexão. O rádio
               acordando a cada minuto custa caro quando falta carga, e a ponte
               volta sozinha — só um pouco mais tarde. */
            herdr_conn_set_retry_cap_ms(pct >= 0 && pct < 20 ? 120000 : 60000);
            low_batt = (pct >= 0 && mv < BATTERY_CUTOFF_MV) ? low_batt + 1 : 0;
            if (low_batt >= 3) {
                say("BATTERY LOW", "charge the pager over USB-C", "then press PWR");
                power_off();
            }
        }

        if (pager_ui_screen() == UI_PAIRING) tick_pairing();

        fw_update_status_t fw;
        fw_update_get_status(&fw);
        if (fw.state == FW_UPDATE_DOWNLOADING) {
            char pct[24];
            snprintf(pct, sizeof(pct), "%u%% - do not unplug", fw.pct);
            pager_ui_set_message("UPDATING", "downloading new firmware", pct);
        }
        pager_ui_set_flags(net_wifi_is_up(), chime_is_muted(), fw.state == FW_UPDATE_AVAILABLE);
        update_status();

        /* sino: só na TRANSIÇÃO para blocked; boot e reconexão ficam mudos (o
         * primeiro payload depois de conectar vira a linha de base) */
        bool online = false;
        for (int i = 0; i < CFG_MAX_HOSTS; i++) online |= herdr_model_get_conn(i) == HERDR_CONN_ONLINE;
        if (online && !was_online) baseline_pending = true;
        was_online = online;

        ui_screen_t scr = pager_ui_screen();
        time_t now = rtc_clock_system_valid() ? time(NULL) : 0;
        bool changed = pager_ui_render(now);
        uint32_t gen = herdr_model_generation();
        if (online && gen != last_gen) {
            int w = pager_ui_waiting();
            if (baseline_pending) baseline_pending = false;
            else if (w > prev_waiting) chime_play(prev_waiting == 0 ? CHIME_BLOCKED : CHIME_MORE);
            prev_waiting = w;
        }
        last_gen = gen;
        if (changed) epd_bsp_flush(scr != last_screen);   /* troca de tela = refresh total */
        last_screen = scr;

        if (s_sntp_synced && !rtc_saved) rtc_saved = rtc_clock_save();
    }
}

/* ------------------------------------------------------------ wi-fi errors */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        uint8_t r = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_wifi_reason = r;
        if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
            s_wifi_auth_failed = true;
        }
        ESP_LOGW(TAG, "STA caiu: reason %u", r);
    }
}

/* os dois casos que o usuário precisa distinguir; o resto vira "não conectou" */
static bool wifi_reason_fatal(uint8_t r)
{
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_NO_AP_FOUND ||
           r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT;
}

/**
 * Espera o IP depois do net_wifi_connect().
 *
 * O net.c re-tenta sozinho a cada 2 s, então um motivo fatal (senha errada,
 * rede inexistente) já se repetiu várias vezes em 6 s — não adianta esperar o
 * teto inteiro. Motivo não fatal (DHCP lento e afins) tem o teto todo.
 */
static bool wait_for_ip(int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 200) {
        if (net_wifi_is_up()) {
            return true;
        }
        if (wifi_reason_fatal(s_wifi_reason) && waited >= 6000) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return net_wifi_is_up();
}

/**
 * Credencial errada não pode terminar em silêncio.
 *
 * Antes: o net.c re-tentava para sempre, o pager ficava offline e sem dizer
 * por quê, e voltar ao portal exigia saber o acorde PWR+BOOT. Agora o motivo
 * vai para a tela e o portal reabre sozinho — corrigir é só salvar de novo.
 */
static void wifi_failed_screen(const char *ssid)
{
    uint8_t r = s_wifi_reason;
    const char *why = "could not connect";
    if (s_wifi_auth_failed || r == WIFI_REASON_AUTH_FAIL ||
        r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
        why = "wrong password?";
    } else if (r == WIFI_REASON_NO_AP_FOUND) {
        why = "network not found";
    }
    ESP_LOGW(TAG, "sem IP (reason %u): %s - abrindo o setup", r, why);
    say("WI-FI FAILED", why, ssid);
    vTaskDelay(pdMS_TO_TICKS(4000));      /* ler antes de a tela de setup trocar */
}

/* ------------------------------------------------------------ boot */
void app_main(void)
{
    board_power_on();                       /* primeiro: segura a trava do VBAT */

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    panel_cfg_init();

    char tz[48];
    if (settings_get_tz(tz, sizeof(tz))) {  /* antes do net.c: ele respeita um TZ já posto */
        setenv("TZ", tz, 1);
        tzset();
    }
    board_i2c_init();
    if (rtc_clock_init()) rtc_clock_restore();

    /* light sleep automático entre eventos; o Wi-Fi fica em modem sleep */
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true };
    if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "gerenciador de energia não configurou");
    battery_init();

    s_btn_q = xQueueCreate(8, sizeof(btn_evt_t));

    /* o modelo tem de existir antes de qualquer render: say() desenha a tela de
       boot e passa por lock(), que pega s_model.mutex. Antes vinha depois de
       say() e o primeiro boot abortava em xQueueSemaphoreTake (mutex NULL).
       Mesma ordem do painel: herdr_model_init() antes do init da UI. */
    herdr_model_init();

    epd_bsp_init();
    say("HERDR PAGER", "herdr-assist " HERDR_ASSIST_VERSION " (epaper154)", "starting...");

    sntp_set_time_sync_notification_cb(on_sntp);
    net_wifi_init();
    /* handler aditivo: o net.c já re-tenta sozinho, isto só registra o motivo */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL);
    if (!chime_init()) ESP_LOGW(TAG, "sem sino (codec não respondeu)");
    chime_set_muted(settings_get_muted());
    buttons_init(on_button);

    bool want_setup = s_boot_into_setup == SETUP_MAGIC;
    s_boot_into_setup = 0;
    if (!panel_cfg_wifi_ok() || want_setup) {
        enter_setup();
    } else {
        const panel_cfg_t *cfg = panel_cfg_get();
        s_wifi_reason = 0;
        s_wifi_auth_failed = false;
        net_wifi_connect(cfg->wifi_ssid, cfg->wifi_pass);
        fw_update_init();                   /* confirma o slot (rollback) e checa 1x/dia */
        if (!wait_for_ip(30000)) {
            /* senha errada / rede ausente: diz o motivo e reabre o portal */
            wifi_failed_screen(cfg->wifi_ssid);
            enter_setup();
        } else if (has_host()) {
            if (herdr_conn_start() == ESP_OK) s_conn_started = true;
            pager_ui_set_screen(UI_MAIN);
        } else {
            start_pairing();
        }
    }

    xTaskCreate(ui_task, "ui", 6144, NULL, 5, NULL);
    ESP_LOGI(TAG, "pager no ar (herdr-assist %s)", HERDR_ASSIST_VERSION);
}
