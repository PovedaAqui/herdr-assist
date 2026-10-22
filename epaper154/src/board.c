#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"

static const char *TAG = "board";
static bool s_i2c_ready;

void board_power_on(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_VBAT_PWR_PIN) | (1ULL << BOARD_EPD_PWR_PIN) |
                        (1ULL << BOARD_AUDIO_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* nível antes da direção: o pino não passa por um estado intermediário */
    gpio_set_level(BOARD_VBAT_PWR_PIN, 1);     /* high = ligado (trava) */
    gpio_set_level(BOARD_EPD_PWR_PIN, 1);      /* ainda desligado */
    gpio_set_level(BOARD_AUDIO_PWR_PIN, 1);    /* ainda desligado */
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_EPD_PWR_PIN, 0);      /* low = ligado */
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "trilhas: VBAT=%d on, EPD=%d on", BOARD_VBAT_PWR_PIN, BOARD_EPD_PWR_PIN);
}

void board_audio_power(bool on)
{
    gpio_set_level(BOARD_AUDIO_PWR_PIN, on ? 0 : 1);
    if (on) vTaskDelay(pdMS_TO_TICKS(20));
}

esp_err_t board_i2c_init(void)
{
    if (s_i2c_ready) return ESP_OK;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_I2C_SDA_PIN,
        .scl_io_num = BOARD_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(BOARD_I2C_PORT, &conf);
    if (err == ESP_OK) err = i2c_driver_install(BOARD_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C não subiu: %s", esp_err_to_name(err));
        return err;
    }
    s_i2c_ready = true;
    return ESP_OK;
}

void board_power_off(void)
{
    gpio_set_level(BOARD_AUDIO_PWR_PIN, 1);
    gpio_set_level(BOARD_EPD_PWR_PIN, 1);
    gpio_set_level(BOARD_VBAT_PWR_PIN, 0);
}
