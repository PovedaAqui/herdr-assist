# Módulo principal do herdr-assist no e-paper 1,54".
#
# Os fontes compartilhados com o painel chegam em src/shared/ no build
# (scripts/sync_shared.py) e são listados explicitamente aqui porque o CMake
# do IDF não gosta de glob em src gerado. Dependências de terceiros só pelo
# component manager (src/idf_component.yml) — nada vendorizado.

set(SHARED_SRCS
    "shared/discovery.c"
    "shared/panel_cfg.c"
    "shared/herdr_model.c"
    "shared/limits_merge.c"
    "shared/activity_log.c"
    "shared/herdr_conn.c"
    "shared/pairing.c"
    "shared/net.c"
    "shared/fw_update.c"
)

# IDF 5.3+ partiu o componente `driver` em esp_driver_*; o painel (e este
# device) fixa espressif32@6.6.0 = IDF 5.2.1, onde tudo ainda mora em `driver`.
set(EPD_DRIVER_REQS driver)
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    list(APPEND EPD_DRIVER_REQS esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_i2s)
endif()

idf_component_register(
    SRCS
        "main.c"
        "board.c"
        "epd_fb.c"
        "epd_bsp.c"
        "pager_ui.c"
        "buttons.c"
        "buttons_machine.c"
        "chime.c"
        "rtc_clock.c"
        "settings.c"
        "portal.c"
        "portal_form.c"
        ${SHARED_SRCS}
    INCLUDE_DIRS "." "shared"
    PRIV_REQUIRES
        ${EPD_DRIVER_REQS}
        esp_lcd
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_server
        esp_http_client
        json
        mbedtls
        esp_https_ota
        app_update
        esp_timer
)
