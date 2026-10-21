/* Sensoriamento dos dois botões (BOOT=GPIO0, PWR=GPIO18 de board_pins.h): as
 * bordas vêm por interrupção com o instante exato, e a classificação mora em
 * buttons_machine.c.
 *
 * Antes era sondagem de nível a cada 40 ms. Na bateria a CPU entra em light
 * sleep entre as amostragens (no USB o CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION
 * segura o sono), então um toque que caía entre duas leituras — ou que só era
 * lido depois do sleep, já solto — nunca virava evento: o PWR não movia o
 * foco. Agora a borda acorda a CPU (gpio_wakeup_enable + gpio wakeup) e a ISR
 * carimba o instante; um tick de 20 ms só deixa debounce/hold correrem. */

#include "buttons.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
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

typedef struct {
    uint8_t id;
    bool pressed;
    int64_t ms;
} btn_edge_t;

#define EDGES 32

static btn_edge_t s_edges[EDGES];
static volatile uint32_t s_head;  /* produtor: ISR */
static volatile uint32_t s_tail;  /* consumidor: scan_task */
static TaskHandle_t s_task;

static btn_cb_t s_cb;
static btn_machine_t s_m;

static void IRAM_ATTR btn_isr(void *arg)
{
    const int id = (int)(intptr_t)arg;
    const uint32_t next = (s_head + 1) % EDGES;
    if (next != s_tail) {              /* se encher, o tick de 20 ms lê o nível */
        s_edges[s_head].id = (uint8_t)id;
        s_edges[s_head].pressed = gpio_get_level(PINS[id]) == 0;
        s_edges[s_head].ms = esp_timer_get_time() / 1000;
        s_head = next;
    }
    BaseType_t woken = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void scan_task(void *arg)
{
    (void)arg;
    bool pressed[2] = { false, false };
    s_task = xTaskGetCurrentTaskHandle();
    for (;;) {
        bool got = false;
        while (s_tail != s_head) {
            const btn_edge_t e = s_edges[s_tail];
            s_tail = (s_tail + 1) % EDGES;
            pressed[e.id] = e.pressed;
            buttons_machine_step(&s_m, pressed, e.ms, s_cb);
            got = true;
        }
        /* bate o passo também sem borda: é ele que fecha o debounce e dispara
         * o HOLD (o botão parado não gera interrupção nenhuma) */
        buttons_machine_step(&s_m, pressed, esp_timer_get_time() / 1000, s_cb);
        if (!got) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
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
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        if (gpio_config(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config(%d) falhou", PINS[i]);
            return false;
        }
    }
    esp_err_t r = gpio_install_isr_service(0);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(r));
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (gpio_isr_handler_add(PINS[i], btn_isr, (void *)(intptr_t)i) != ESP_OK) {
            ESP_LOGE(TAG, "gpio_isr_handler_add(%d) falhou", PINS[i]);
            return false;
        }
        /* o aperto tem de acordar o light sleep: dormindo, a ISR não roda */
        gpio_wakeup_enable(PINS[i], GPIO_INTR_LOW_LEVEL);
    }
    r = esp_sleep_enable_gpio_wakeup();
    if (r != ESP_OK) ESP_LOGE(TAG, "esp_sleep_enable_gpio_wakeup: %s", esp_err_to_name(r));

    buttons_machine_reset(&s_m, esp_timer_get_time() / 1000);
    /* o PWR que ligou a placa pode ainda estar apertado: não vira evento */
    for (int i = 0; i < 2; i++) {
        if (gpio_get_level(PINS[i]) == 0) {
            s_m.raw[i] = s_m.down[i] = true;
            s_m.consumed[i] = true;
        }
    }
    xTaskCreate(scan_task, "buttons", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "botões prontos por borda (PWR=%d, BOOT=%d)", PINS[BTN_PWR], PINS[BTN_BOOT]);
    return true;
}
