# Radio backend by target (board.h has the matching pin map):
# ESP32 = DevKit with a CC1101, ESP32-C3 = LR1121 receiver board.
if(IDF_TARGET STREQUAL "esp32c3")
    set(radio_src "radio_lr1121.c")
else()
    set(radio_src "radio_cc1101.c")
endif()

idf_component_register(
    SRCS "main.c" "radio.c" ${radio_src} "tpms_ford.c" "sensors.c" "display.c" "gfx.c" "ui.c" "cli.c"
         "led.c" "net.c" "web.c" "ha_mqtt.c" "settings.c" "tires.c" "ota.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html" "web/oled.html"
    PRIV_REQUIRES console esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart esp_driver_rmt
                  esp_timer esp_app_format esp_pm esp_wifi esp_netif esp_event nvs_flash esp_http_server
                  app_update bootloader_support esp_rom)
