/* O sino: ES8311 + I2S, direto no espressif/esp_codec_dev (Apache-2.0).
 *
 * Sem asset nenhum: o bipe é sintetizado em runtime. A ordem importa — o I2S
 * (e com ele o MCLK) sobe ANTES de falar I2C com o ES8311, que não responde
 * sem clock. Pinos e PA do board_cfg.txt do exemplo oficial (entrada
 * S3_ePaper_1_54): mclk=14 bclk=15 ws=38 dout=45, PA=46, use_mclk=1.
 *
 * Toca numa task própria: um bipe de 2-3 tons leva ~400 ms e não pode travar
 * a UI nem os botões. Bateria: o codec só fica aberto enquanto toca —
 * esp_codec_dev_close desliga o PA e o canal I2S, e um I2S ligado seguraria
 * o lock de PM e impediria o light sleep.
 *
 * PENDENTE DE BANCADA: volume (CHIME_VOL) e se o PA é ativo em nível alto
 * (pa_reverted=false, como o codec_board do vendor assume).
 */

#include "chime.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board.h"
#include "board_pins.h"

static const char *TAG = "chime";

#define CHIME_RATE_HZ   16000
#define CHIME_TONE_MS   110
#define CHIME_GAP_MS    40
#define CHIME_SAMPLES   (CHIME_RATE_HZ * CHIME_TONE_MS / 1000)
#ifndef CHIME_VOL
#define CHIME_VOL       75
#endif

static esp_codec_dev_handle_t s_out;
static QueueHandle_t s_queue;
static volatile bool s_muted;
static int16_t s_buf[CHIME_SAMPLES * 2];   /* estéreo entrelaçado L/R */

static void synth_tone(float freq_hz)
{
    const int ramp = CHIME_RATE_HZ / 200;  /* 5 ms de envelope: sem estalo */
    for (int i = 0; i < CHIME_SAMPLES; i++) {
        float env = 1.0f;
        if (i < ramp) env = (float)i / ramp;
        else if (i > CHIME_SAMPLES - ramp) env = (float)(CHIME_SAMPLES - i) / ramp;
        int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * freq_hz * i / CHIME_RATE_HZ) * env * 12000.0f);
        s_buf[2 * i] = v;
        s_buf[2 * i + 1] = v;
    }
}

static void chime_task(void *arg)
{
    (void)arg;
    static const float seq_blocked[] = { 880.0f, 1175.0f };
    static const float seq_more[]    = { 880.0f, 1175.0f, 1480.0f };
    static const float seq_test[]    = { 1175.0f };
    chime_kind_t kind;
    for (;;) {
        if (xQueueReceive(s_queue, &kind, portMAX_DELAY) != pdTRUE) continue;
        const float *seq = kind == CHIME_MORE ? seq_more : kind == CHIME_TEST ? seq_test : seq_blocked;
        int n = kind == CHIME_MORE ? 3 : kind == CHIME_TEST ? 1 : 2;
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = CHIME_RATE_HZ,
            .channel = 2,
            .bits_per_sample = 16,
        };
        if (esp_codec_dev_open(s_out, &fs) != ESP_CODEC_DEV_OK) continue;
        esp_codec_dev_set_out_vol(s_out, CHIME_VOL);
        for (int i = 0; i < n; i++) {
            synth_tone(seq[i]);
            esp_codec_dev_write(s_out, s_buf, sizeof(s_buf));
            vTaskDelay(pdMS_TO_TICKS(CHIME_GAP_MS));
        }
        esp_codec_dev_close(s_out);
    }
}

bool chime_init(void)
{
    board_audio_power(true);
    if (board_i2c_init() != ESP_OK) return false;

    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, &tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "canal I2S não subiu");
        return false;
    }
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CHIME_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_PIN,
            .bclk = BOARD_I2S_BCLK_PIN,
            .ws = BOARD_I2S_LRCK_PIN,
            .dout = BOARD_I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));
    /* MCLK correndo enquanto o ES8311 é configurado por I2C (ordem do
     * exemplo oficial); depois o canal é desligado e quem liga/desliga é o
     * esp_codec_dev, a cada open/close */
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = BOARD_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_I2S_PA_CTRL,
        .pa_reverted = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es);
    i2s_channel_disable(tx);
    if (!data_if || !ctrl_if || !gpio_if || !codec_if) {
        ESP_LOGE(TAG, "ES8311 não respondeu (I2C 0x%02X)", BOARD_ES8311_ADDR);
        return false;
    }
    esp_codec_dev_cfg_t dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_out = esp_codec_dev_new(&dev);
    if (!s_out) {
        ESP_LOGE(TAG, "esp_codec_dev_new falhou");
        return false;
    }
    /* o es8311_codec_new deixa o PA ligado; um open/close o desliga até o
     * primeiro bipe */
    esp_codec_dev_sample_info_t fs = { .sample_rate = CHIME_RATE_HZ, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_out, &fs) == ESP_CODEC_DEV_OK) esp_codec_dev_close(s_out);

    s_queue = xQueueCreate(4, sizeof(chime_kind_t));
    xTaskCreate(chime_task, "chime", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ES8311 pronto (%d Hz, vol %d)", CHIME_RATE_HZ, CHIME_VOL);
    return true;
}

void chime_play(chime_kind_t kind)
{
    if (!s_queue || (s_muted && kind != CHIME_TEST)) return;
    xQueueSend(s_queue, &kind, 0);   /* fila cheia = já tem bipe tocando */
}

void chime_set_muted(bool muted) { s_muted = muted; }
bool chime_is_muted(void) { return s_muted; }
