/* Leitura do ADC da bateria — ver battery.h. */

#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "batt";

#define BATT_ADC_UNIT    ADC_UNIT_1
#define BATT_ADC_CHANNEL ADC_CHANNEL_3     /* GPIO4 */
#define BATT_DIVIDER     2

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

bool battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = BATT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) return false;
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &ch) != ESP_OK) return false;
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = BATT_ADC_UNIT,
        .chan = BATT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "sem calibração do ADC: leitura crua");
        s_cali = NULL;
    }
    return true;
}

int battery_read_mv(void)
{
    if (!s_adc) return -1;
    int sum = 0, ok = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3300 / 4095;
        ok++;
    }
    return ok ? sum / ok * BATT_DIVIDER : -1;
}
