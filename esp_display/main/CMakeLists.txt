idf_component_register(
    SRCS "main.c" "image.c"
    PRIV_REQUIRES esp_driver_spi esp_driver_uart
    INCLUDE_DIRS "")
