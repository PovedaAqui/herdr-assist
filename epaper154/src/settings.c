#include "settings.h"

#include <string.h>

#include "nvs.h"

#define NS "pager"

bool settings_get_tz(char *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = len;
    esp_err_t err = nvs_get_str(h, "tz", out, &n);
    nvs_close(h);
    return err == ESP_OK && out[0];
}

void settings_set_tz(const char *tz)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "tz", tz ? tz : "");
    nvs_commit(h);
    nvs_close(h);
}

bool settings_get_muted(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, "muted", &v);
    nvs_close(h);
    return v != 0;
}

void settings_set_muted(bool muted)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "muted", muted ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}
