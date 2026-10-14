idf_component_register(
    SRCS "main.c" "radio.c" "tpms_ford.c" "sensors.c" "display.c" "gfx.c" "ui.c" "cli.c" "battery.c"
         "net.c" "web.c" "ha_mqtt.c" "settings.c" "tires.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES console esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart esp_adc esp_timer
                  esp_app_format esp_pm esp_wifi esp_netif esp_event nvs_flash esp_http_server)
