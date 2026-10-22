/* Varredura dos dois botões (10 ms ativa, 20 ms parada); a classificação mora
 * em buttons_machine.c. Pinos: BOOT=GPIO0, PWR=GPIO18 (board_pins.h).
 *
 * Era interrupção de borda, para o aperto acordar a CPU no light sleep. O light
 * sleep está desligado (ver main.c), então a ISR só trouxe risco: um toque com
 * bounce gerava uma tempestade de bordas e o watchdog de interrupção matava a
 * CPU — "Interrupt wdt timeout on CPU0", com o backtrace em gpio_isr_loop.
 * Sondagem de 10 ms não perde toque nenhum (o debounce é 30 ms) e não tem ISR.
 */

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
        /* apertado ou mudando: 10 ms; parado: 20 ms (não precisa de ISR) */
        bool busy = pressed[0] || pressed[1] || s_m.down[0] || s_m.down[1] ||
                    s_m.raw[0] != s_m.down[0] || s_m.raw[1] != s_m.down[1];
        vTaskDelay(pdMS_TO_TICKS(busy ? 10 : 20));
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
    ESP_LOGI(TAG, "botões prontos por sondagem (PWR=%d, BOOT=%d)", PINS[BTN_PWR], PINS[BTN_BOOT]);
    return true;
}
