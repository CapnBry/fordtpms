# Reuses the receiver firmware's Ford encoder/decoder.
idf_component_register(
    SRCS "main.c" "../../../src/tpms_ford.c"
    INCLUDE_DIRS "." "../../../src"
    PRIV_REQUIRES console esp_driver_gpio esp_driver_spi esp_driver_uart esp_driver_dac esp_timer)
