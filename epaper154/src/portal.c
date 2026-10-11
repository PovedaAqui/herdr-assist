/* Ponto de acesso + servidor HTTP da configuração do Wi-Fi — ver portal.h. */

#include "portal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "net.h"
#include "panel_cfg.h"
#include "settings.h"

static const char *TAG = "portal";

/* Fusos POSIX mais comuns; o primeiro é o padrão do herdr-assist (BRT). */
static const struct { const char *label, *tz; } TZS[] = {
    { "Brasília / Buenos Aires (UTC-3)", "<-03>3" },
    { "UTC", "UTC0" },
    { "London / Lisbon", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Madrid / Paris / Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Athens / Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "New York", "EST5EDT,M3.2.0,M11.1.0" },
    { "Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "Mexico City", "CST6" },
    { "India", "IST-5:30" },
    { "Singapore / Shanghai", "<+08>-8" },
    { "Tokyo", "JST-9" },
    { "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};

static net_ap_t s_aps[16];
static int s_ap_count;
static httpd_handle_t s_httpd;

/* escreve s com <>&" escapados — SSID é texto de terceiro */
static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[200];
    size_t n = 0;
    for (; *s && n < sizeof(buf) - 7; s++) {
        const char *e = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (e) { strcpy(buf + n, e); n += strlen(e); }
        else buf[n++] = *s;
    }
    buf[n] = '\0';
    httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t page_get(httpd_req_t *req)
{
    char cur_tz[48] = "";
    settings_get_tz(cur_tz, sizeof(cur_tz));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
        "<title>herdr pager</title><style>body{font:16px system-ui;margin:24px;max-width:420px}"
        "input,select,button{font:inherit;width:100%;padding:8px;margin:4px 0 14px;box-sizing:border-box}"
        "</style></head><body><h2>herdr pager &middot; Wi-Fi</h2>"
        "<form method=post action=/save><label>Network</label>"
        "<input name=ssid list=aps required maxlength=32 autocomplete=off><datalist id=aps>");
    for (int i = 0; i < s_ap_count; i++) {
        httpd_resp_sendstr_chunk(req, "<option value=\"");
        send_escaped(req, s_aps[i].ssid);
        httpd_resp_sendstr_chunk(req, "\">");
    }
    httpd_resp_sendstr_chunk(req,
        "</datalist><label>Password</label><input name=pass type=password maxlength=64>"
        "<label>Time zone</label><select name=tz>");
    for (size_t i = 0; i < sizeof(TZS) / sizeof(TZS[0]); i++) {
        char opt[160];
        snprintf(opt, sizeof(opt), "<option value=\"%u\"%s>%s</option>", (unsigned)i,
                 !strcmp(cur_tz, TZS[i].tz) ? " selected" : "", TZS[i].label);
        httpd_resp_sendstr_chunk(req, opt);
    }
    httpd_resp_sendstr_chunk(req,
        "</select><button>Save and restart</button></form>"
        "<p>Next step after the restart: pair with your Herdr machine "
        "(the pager shows how).</p></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void restart_cb(void *arg) { (void)arg; esp_restart(); }

static esp_err_t save_post(httpd_req_t *req)
{
    char body[400];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    char ssid[CFG_SSID_LEN] = "", pass[CFG_PASS_LEN] = "", tzi[8] = "";
    size_t plen;
    if (!portal_form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0] ||
        (portal_form_get(body, "pass", pass, sizeof(pass)) && (plen = strlen(pass)) > 0 && plen < 8)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Network name required; WPA passwords have 8 to 64 characters.");
        return ESP_OK;
    }
    panel_cfg_t cfg = *panel_cfg_get();
    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid);
    snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", pass);
    panel_cfg_save(&cfg);
    if (portal_form_get(body, "tz", tzi, sizeof(tzi))) {
        int i = atoi(tzi);
        if (i >= 0 && i < (int)(sizeof(TZS) / sizeof(TZS[0]))) settings_set_tz(TZS[i].tz);
    }
    ESP_LOGI(TAG, "Wi-Fi salvo (%s); reiniciando", ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<!doctype html><body style='font:16px system-ui;margin:24px'>"
                            "<h2>Saved</h2><p>The pager is restarting and will join your network. "
                            "You can leave this Wi-Fi now.</p></body>");
    /* responde antes de cair: o reinício sai do timer, não do handler */
    const esp_timer_create_args_t t = { .callback = restart_cb, .name = "portal_rst" };
    esp_timer_handle_t h;
    if (esp_timer_create(&t, &h) == ESP_OK) esp_timer_start_once(h, 1500 * 1000);
    return ESP_OK;
}

esp_err_t portal_start(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    /* varre ANTES de abrir o AP: em APSTA o rádio fica preso ao canal do AP */
    s_ap_count = net_wifi_scan(s_aps, sizeof(s_aps) / sizeof(s_aps[0]));
    ESP_LOGI(TAG, "%d redes à vista", s_ap_count);

    esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "herdr-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%08lu",
             (unsigned long)(esp_random() % 100000000UL));
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t get = { .uri = "/", .method = HTTP_GET, .handler = page_get };
    const httpd_uri_t post = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(s_httpd, &get);
    httpd_register_uri_handler(s_httpd, &post);

    snprintf(ssid_out, ssid_len, "%s", (char *)ap.ap.ssid);
    snprintf(pass_out, pass_len, "%s", (char *)ap.ap.password);
    ESP_LOGI(TAG, "AP %s aberto; http://" PORTAL_URL, ssid_out);
    return ESP_OK;
}
