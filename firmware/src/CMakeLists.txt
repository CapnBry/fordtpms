idf_component_register(
    SRCS "main.c" "radio.c" "tpms_ford.c" "sensors.c" "display.c" "gfx.c" "ui.c" "cli.c" "battery.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart esp_adc esp_timer
                  esp_app_format esp_pm)
