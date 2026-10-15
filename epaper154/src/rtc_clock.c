/* PCF85063A: registradores de tempo em 0x04..0x0A (BCD), Control_1 em 0x00
 * (bit 5 = STOP). Bit 7 do registrador de segundos = OS (o oscilador parou —
 * a hora guardada não vale). O RTC guarda UTC; fuso é coisa do sistema. */

#include "rtc_clock.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/i2c.h"
#include "esp_log.h"

#include "board.h"
#include "board_pins.h"

static const char *TAG = "rtc";

#define REG_CTRL1   0x00
#define REG_SECONDS 0x04

static bool s_ready;

static inline int bcd2dec(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }
static inline uint8_t dec2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_write_read_device(BOARD_I2C_PORT, BOARD_RTC_ADDR, &reg, 1, out, len,
                                        pdMS_TO_TICKS(100));
}

static esp_err_t wr(const uint8_t *buf, size_t len)
{
    return i2c_master_write_to_device(BOARD_I2C_PORT, BOARD_RTC_ADDR, buf, len, pdMS_TO_TICKS(100));
}

bool rtc_clock_init(void)
{
    if (board_i2c_init() != ESP_OK) return false;
    uint8_t ctrl1 = 0;
    if (rd(REG_CTRL1, &ctrl1, 1) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A não respondeu em 0x%02X", BOARD_RTC_ADDR);
        return false;
    }
    if (ctrl1 & 0x20) {
        uint8_t b[2] = { REG_CTRL1, (uint8_t)(ctrl1 & ~0x20) };
        wr(b, sizeof(b));
        ESP_LOGW(TAG, "RTC estava parado; religado");
    }
    s_ready = true;
    return true;
}

bool rtc_clock_system_valid(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return tm.tm_year + 1900 >= 2024;
}

bool rtc_clock_restore(void)
{
    if (!s_ready) return false;
    uint8_t t[7];
    if (rd(REG_SECONDS, t, sizeof(t)) != ESP_OK) return false;
    if (t[0] & 0x80) {
        ESP_LOGW(TAG, "RTC perdeu a hora (OS); espera o SNTP");
        return false;
    }
    struct tm tm = {
        .tm_sec = bcd2dec(t[0] & 0x7F),
        .tm_min = bcd2dec(t[1] & 0x7F),
        .tm_hour = bcd2dec(t[2] & 0x3F),
        .tm_mday = bcd2dec(t[3] & 0x3F),
        .tm_mon = bcd2dec(t[5] & 0x1F) - 1,
        .tm_year = bcd2dec(t[6]) + 100,
    };
    /* mktime interpreta como hora local: calcula em UTC trocando o TZ */
    char *old = getenv("TZ");
    char saved[64] = "";
    if (old) strncpy(saved, old, sizeof(saved) - 1);
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&tm);
    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();
    if (epoch <= 0) return false;
    struct timeval tv = { .tv_sec = epoch };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "hora restaurada do RTC: %04d-%02d-%02d %02d:%02d UTC", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return true;
}

bool rtc_clock_save(void)
{
    if (!s_ready || !rtc_clock_system_valid()) return false;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    uint8_t b[8] = {
        REG_SECONDS,
        dec2bcd(tm.tm_sec),            /* escrever os segundos zera o bit OS */
        dec2bcd(tm.tm_min),
        dec2bcd(tm.tm_hour),
        dec2bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday,
        dec2bcd(tm.tm_mon + 1),
        dec2bcd(tm.tm_year % 100),
    };
    bool ok = wr(b, sizeof(b)) == ESP_OK;
    ESP_LOGI(TAG, "RTC %s", ok ? "acertado pelo SNTP" : "não aceitou a escrita");
    return ok;
}
