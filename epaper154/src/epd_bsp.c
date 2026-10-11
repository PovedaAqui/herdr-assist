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

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1681.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_pins.h"
#include "ssd1681_lut_1in54_v2.h"

static const char *TAG = "epd";

#ifndef EPD_MIRROR_X
#define EPD_MIRROR_X false
#endif
#ifndef EPD_MIRROR_Y
#define EPD_MIRROR_Y false
#endif
#define EPD_SPI_HZ        (10 * 1000 * 1000)
#define EPD_REFRESH_TMO   pdMS_TO_TICKS(6000)

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_done;
static bool s_ready;
static bool s_fast_lut;      /* a LUT carregada agora é a rápida? */
static int  s_fast_count;
static int  s_flushes;
static int  s_last_ms;

static bool IRAM_ATTR on_refresh_done(const esp_lcd_panel_handle_t h, const void *edata, void *ctx)
{
    (void)h; (void)edata;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
}

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

    s_done = xSemaphoreCreateBinary();
    epaper_panel_callbacks_t cbs = { .on_epaper_refresh_done = on_refresh_done };
    epaper_panel_register_event_callbacks(s_panel, &cbs, s_done);

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

void epd_bsp_flush(bool full)
{
    if (!s_ready) return;
    int64_t t0 = esp_timer_get_time();
    full = full || s_fast_count >= EPD_FAST_BUDGET;

    if (full && s_fast_lut) {
        esp_lcd_panel_disp_on_off(s_panel, true);     /* volta à LUT da OTP */
        s_fast_lut = false;
    } else if (!full && !s_fast_lut) {
        epaper_panel_set_custom_lut(s_panel, SSD1681_WAVESHARE_1IN54_V2_LUT_FAST_REFRESH, 159);
        s_fast_lut = true;
    }

    /* só a RAM preta, como o exemplo do componente faz no refresh rápido; se no
     * vidro o rápido sujar, o próximo passo é escrever a tela anterior na RAM
     * "vermelha" (0x26), que é a referência do waveform — PENDENTE DE BANCADA */
    epaper_panel_set_bitmap_color(s_panel, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, EPD_W, EPD_H, epd_fb());
    xSemaphoreTake(s_done, 0);                        /* zera um give velho */
    epaper_panel_refresh_screen(s_panel);
    if (xSemaphoreTake(s_done, EPD_REFRESH_TMO) != pdTRUE)
        ESP_LOGW(TAG, "refresh sem fim de BUSY em %d ms", (int)pdTICKS_TO_MS(EPD_REFRESH_TMO));

    s_fast_count = full ? 0 : s_fast_count + 1;
    s_flushes++;
    s_last_ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "refresh %s em %d ms (rápidos desde o total: %d)", full ? "total" : "rápido",
             s_last_ms, s_fast_count);
}

int epd_bsp_flush_count(void) { return s_flushes; }
int epd_bsp_last_flush_ms(void) { return s_last_ms; }

void epd_bsp_sleep(void)
{
    if (!s_ready) return;
    esp_lcd_panel_disp_on_off(s_panel, false);        /* deep sleep do SSD1681 */
    s_ready = false;
}
