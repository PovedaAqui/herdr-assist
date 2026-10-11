cmake_minimum_required(VERSION 3.16.0)

# A Waveshare vende duas revisões da placa: V2 (ESP32-S3-PICO-1-N8R8, PSRAM
# octal) e V1 (ESP32-S3FH4R2, PSRAM quad). O platformio.ini escolhe com
# -DPAGER_VARIANT=v1|v2; o resto da configuração é comum.
if(NOT DEFINED PAGER_VARIANT)
    set(PAGER_VARIANT v2)
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${PAGER_VARIANT}")

# Versão do app (esp_app_desc): o fw_update compara com a do manifest via
# strcmp. O IDF só procura version.txt na pasta do projeto, mas o CI grava o
# da tag na raiz do repositório — sem isto o pager se veria sempre
# desatualizado. Fora do CI, o git describe de sempre.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../version.txt")
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../version.txt" PROJECT_VER LIMIT_COUNT 1)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper154)
