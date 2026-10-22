/* O vidro: SSD1681 via espressif/esp_lcd_ssd1681.
 *
 * Refresh total = LUT de fábrica do painel (o disp_on_off(true) do driver
 * recarrega a da OTP): pisca, ~2 s, limpa o ghosting. Refresh rápido = LUT de
 * refresh rápido da Waveshare 1.54" V2 (do exemplo do próprio componente, ver
 * ssd1681_lut_1in54_v2.h): sem o pisca. O exemplo avisa que, depois de vários
 * rápidos, o branco acinzenta — é o que o EPD_FAST_BUDGET resolve, forçando um
 * total a cada N.
 *
 * PENDENTE DE BANCADA: orientação (EPD_MIRROR_X/Y), tempo real de cada modo e
 * o orçamento de rápidos. Os valores aqui são os do exemplo, não medidos.
 */

#include "epd_bsp.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1681.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_pins.h"
#include "ssd1681_lut_1in54_v2.h"

static const char *TAG = "epd";

/* Comandos que o componente não expõe */
#define SSD1681_CMD_BORDER_WAVEFORM       0x3C
#define SSD1681_CMD_MASTER_ACTIVATION     0x20
#define SSD1681_CMD_UPDATE_CTRL2          0x22

#ifndef EPD_MIRROR_X
#define EPD_MIRROR_X false
#endif
#ifndef EPD_MIRROR_Y
#define EPD_MIRROR_Y false
#endif
#define EPD_SPI_HZ        (10 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;   /* comandos que o componente não expõe */
/* durante o refresh o BUSY precisa acordar a CPU: sem light sleep até ele cair */
static esp_pm_lock_handle_t s_pm_lock;
static bool s_ready;
/* Atualização parcial ligada: modo parcial do fabricante + 0xCF com a LUT
 * rápida. O total (LUT da OTP + 0xC7) continua sendo o primeiro quadro, a
 * troca de tela e a limpeza de fantasma a cada EPD_FAST_BUDGET parciais. */
#ifndef EPD_FAST_REFRESH
#define EPD_FAST_REFRESH 1
#endif
static int  s_fast_count;
static int  s_flushes;
static int  s_last_ms;

bool epd_bsp_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_EPD_MOSI_PIN,
        .sclk_io_num = BOARD_EPD_SCK_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_FB_BYTES + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_EPD_DC_PIN,
        .cs_gpio_num = BOARD_EPD_CS_PIN,
        .pclk_hz = EPD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_EPD_SPI_HOST,
                                             &io_cfg, &io));
    s_io = io;

    esp_lcd_ssd1681_config_t vendor = {
        .busy_gpio_num = BOARD_EPD_BUSY_PIN,
        .non_copy_mode = false,
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BOARD_EPD_RST_PIN,
        .flags.reset_active_high = false,
        .vendor_config = &vendor,
    };
    /* o driver registra um handler no BUSY: o serviço de ISR vem antes */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1681(io, &dev, &s_panel));

    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epd", &s_pm_lock);

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    esp_lcd_panel_mirror(s_panel, EPD_MIRROR_X, EPD_MIRROR_Y);
    esp_lcd_panel_invert_color(s_panel, false);

    s_ready = true;
    epd_fb_clear();
    /* como o exemplo do componente: zera também a RAM "vermelha" uma vez */
    epaper_panel_set_bitmap_color(s_panel, SSD1681_EPAPER_BITMAP_RED);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, EPD_W, EPD_H, epd_fb());
    epd_bsp_flush(true);
    ESP_LOGI(TAG, "painel pronto (%dx%d), primeiro refresh em %d ms", EPD_W, EPD_H, s_last_ms);
    return true;
}

/* Receita do driver SSD1683 do ESPHome. O ponto que faltava é o 0x21: o
 * datasheet (Tab. 7-1) define [7:4] como opção da RAM vermelha — 0000 normal,
 * 0100 bypass como 0, 1000 INVERSO — e [3:0] igual para a preta. Ou seja, o
 * inverso é feito HARDWARE: escrever o inverso à mão na 0x26, como eu fazia,
 * inverte duas vezes e a comparação vira o contrário a cada passo (a tela
 * trocava de polaridade a cada dois apertos e voltava).
 *
 *   0x3C  0x80 parcial / 0x01 total   (borda)
 *   0x21  0x80 parcial / 0x40 total   (vermelha invertida / bypass)
 *   0x22  0xFC parcial / 0xF7 total   (0xFC carrega a LUT de fábrica no modo 2)
 *   RAM   0x24 e 0x26 levam o quadro como está, nos dois modos
 */
static bool wait_busy(void)
{
    const int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(BOARD_EPD_BUSY_PIN) && (esp_timer_get_time() - t0) < 6000000)
        vTaskDelay(pdMS_TO_TICKS(5));
    vTaskDelay(pdMS_TO_TICKS(4));                 /* folga, como no exemplo */
    return gpio_get_level(BOARD_EPD_BUSY_PIN) == 0;
}

static uint8_t s_inverse[EPD_FB_BYTES];

static void epd_cmd(uint8_t cmd, const uint8_t *data, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, data, n);
}

bool epd_bsp_flush(bool full)
{
    if (!s_ready) return true;      /* sem painel não há o que pintar */
    int64_t t0 = esp_timer_get_time();
    if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock);
    full = full || !EPD_FAST_REFRESH || s_fast_count >= EPD_FAST_BUDGET;

    epd_cmd(SSD1681_CMD_BORDER_WAVEFORM, (uint8_t[]){ full ? 0x01 : 0x80 }, 1);
    /* [7:4] = RAM vermelha invertida nos dois modos: assim o par
     * (vermelha, preta) sempre difere em todo pixel — o diferencial dirige o
     * quadro inteiro e não sobra resíduo, no parcial e no total */
    epd_cmd(0x21, (uint8_t[]){ 0x80, 0x00 }, 2);

    /* A LUT de fábrica do modo 2 (0xFC, a do parcial) tem polaridade de dados
     * oposta à do modo 1 (0xF7, a do total): com o quadro como está, o parcial
     * pinta o negativo e cada passo inverte tudo. Mandamos o quadro invertido
     * no parcial — o resultado no vidro fica igual ao do total.
     * Com 0x21=0x80 a vermelha é invertida em hardware, então ela vê o
     * contrário da preta e todo pixel conta como "mudou" (é o que limpa o
     * resíduo, sem precisar de refresh total a cada aperto). */
    /* o painel lê os dados ao contrário da nossa convenção (1=preto): vale
     * para as duas LUTs de fábrica, então a inversão vai nos dois modos */
    for (int i = 0; i < EPD_FB_BYTES; i++) s_inverse[i] = (uint8_t)~epd_fb()[i];
    const uint8_t *bw = s_inverse;
    epaper_panel_set_bitmap_color(s_panel, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, EPD_W, EPD_H, bw);
    epaper_panel_set_bitmap_color(s_panel, SSD1681_EPAPER_BITMAP_RED);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, EPD_W, EPD_H, bw);

    epd_cmd(SSD1681_CMD_UPDATE_CTRL2, (uint8_t[]){ full ? 0xF7 : 0xFC }, 1);
    epd_cmd(SSD1681_CMD_MASTER_ACTIVATION, NULL, 0);
    bool ok = wait_busy();
    if (!ok) ESP_LOGW(TAG, "refresh sem fim de BUSY (6 s)");

    if (s_pm_lock) esp_pm_lock_release(s_pm_lock);
    s_fast_count = full ? 0 : s_fast_count + 1;
    s_flushes++;
    s_last_ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "refresh %s em %d ms (rápidos desde o total: %d)%s", full ? "total" : "rápido",
             s_last_ms, s_fast_count, ok ? "" : " — NAO PINTou");
    return ok;
}

int epd_bsp_flush_count(void) { return s_flushes; }
int epd_bsp_last_flush_ms(void) { return s_last_ms; }

void epd_bsp_sleep(void)
{
    if (!s_ready) return;
    esp_lcd_panel_disp_on_off(s_panel, false);        /* deep sleep do SSD1681 */
    s_ready = false;
}
