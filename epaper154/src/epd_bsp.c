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
#define SSD1681_CMD_WRITE_REG_FOR_DISPLAY 0x37
#define SSD1681_CMD_BORDER_WAVEFORM       0x3C
#define SSD1681_CMD_END_OPTION            0x3F
#define SSD1681_CMD_GATE_VOLTAGE          0x03
#define SSD1681_CMD_SOURCE_VOLTAGE        0x04
#define SSD1681_CMD_VCOM_REGISTER         0x2C
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

/* Entra no modo parcial como o EPD_1in54_V2_Init_Partial() do fabricante:
 * carrega a LUT da atualização rápida, escreve os 10 bytes do 0x37, o 0x3C=0x80
 * (borda seguindo a LUT) e liga clock/analógico com 0x22=0xC0. Sem esses três
 * registradores o 0xCF não faz atualização diferencial — foi o que fez o vidro
 * desbotar e, no caso da janela 0x91, ficar em branco. */
static bool s_partial_mode;

static bool wait_busy(void)
{
    const int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(BOARD_EPD_BUSY_PIN) && (esp_timer_get_time() - t0) < 6000000)
        vTaskDelay(pdMS_TO_TICKS(5));
    vTaskDelay(pdMS_TO_TICKS(4));                 /* folga, como no exemplo */
    return gpio_get_level(BOARD_EPD_BUSY_PIN) == 0;
}

static void epd_cmd(uint8_t cmd, const uint8_t *data, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, data, n);
}

static void enter_partial_mode(void)
{
    if (s_partial_mode) return;
    uint8_t *lut = SSD1681_WAVESHARE_1IN54_V2_LUT_FAST_REFRESH;
    epd_cmd(0x32, lut, 153);                      /* LUT do refresh rápido */
    epd_cmd(SSD1681_CMD_END_OPTION, &lut[153], 1);
    epd_cmd(SSD1681_CMD_GATE_VOLTAGE, &lut[154], 1);
    epd_cmd(SSD1681_CMD_SOURCE_VOLTAGE, &lut[155], 3);
    epd_cmd(SSD1681_CMD_VCOM_REGISTER, &lut[158], 1);

    static const uint8_t reg37[10] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00 };
    epd_cmd(SSD1681_CMD_WRITE_REG_FOR_DISPLAY, reg37, sizeof(reg37));
    epd_cmd(SSD1681_CMD_BORDER_WAVEFORM, (uint8_t[]){ 0x80 }, 1);

    epd_cmd(SSD1681_CMD_UPDATE_CTRL2, (uint8_t[]){ 0xC0 }, 1);   /* só liga clock/analógico */
    epd_cmd(SSD1681_CMD_MASTER_ACTIVATION, NULL, 0);
    wait_busy();
    s_partial_mode = true;
}

bool epd_bsp_flush(bool full)
{
    if (!s_ready) return true;      /* sem painel não há o que pintar */
    int64_t t0 = esp_timer_get_time();
    if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock);
    full = full || !EPD_FAST_REFRESH || s_fast_count >= EPD_FAST_BUDGET;

    if (full) {
        if (s_partial_mode) {
            esp_lcd_panel_disp_on_off(s_panel, true); /* volta à LUT da OTP */
            s_partial_mode = false;
        }
    } else {
        enter_partial_mode();
    }

    /* Como o DisplayPart()/DisplayPartBaseImage() do fabricante: a preta (0x24)
     * leva sempre a imagem nova; a vermelha (0x26) só é reescrita no refresh
     * total (ela é a referência do diferencial, e o controlador a atualiza
     * sozinho ao fim de cada parcial). */
    epaper_panel_set_bitmap_color(s_panel, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, EPD_W, EPD_H, epd_fb());
    if (full) {
        epaper_panel_set_bitmap_color(s_panel, SSD1681_EPAPER_BITMAP_RED);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, EPD_W, EPD_H, epd_fb());
    }
    epd_cmd(SSD1681_CMD_UPDATE_CTRL2, (uint8_t[]){ full ? 0xC7 : 0xCF }, 1);
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
