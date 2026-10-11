/* Varredura dos dois botões a cada 10 ms; a classificação mora em
 * buttons_machine.c. Pinos: BOOT=GPIO0, PWR=GPIO18 (board_pins.h). */

#include "buttons.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"

static const char *TAG = "btn";

/* indexado por btn_id_t: [BTN_PWR] e [BTN_BOOT] */
static const gpio_num_t PINS[2] = {
    [BTN_PWR] = BOARD_PWR_BTN_PIN,
    [BTN_BOOT] = BOARD_BOOT_BTN_PIN,
};

static btn_cb_t s_cb;
static btn_machine_t s_m;

static void scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool pressed[2];
        for (int i = 0; i < 2; i++) pressed[i] = gpio_get_level(PINS[i]) == 0;
        buttons_machine_step(&s_m, pressed, esp_timer_get_time() / 1000, s_cb);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool buttons_init(btn_cb_t cb)
{
    s_cb = cb;
    for (int i = 0; i < 2; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << PINS[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config(%d) falhou", PINS[i]);
            return false;
        }
    }
    buttons_machine_reset(&s_m, esp_timer_get_time() / 1000);
    /* o PWR que ligou a placa pode ainda estar apertado: não vira evento */
    for (int i = 0; i < 2; i++) {
        if (gpio_get_level(PINS[i]) == 0) {
            s_m.raw[i] = s_m.down[i] = true;
            s_m.consumed[i] = true;
        }
    }
    xTaskCreate(scan_task, "buttons", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "botões prontos (PWR=%d, BOOT=%d)", PINS[BTN_PWR], PINS[BTN_BOOT]);
    return true;
}
