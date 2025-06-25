idf_component_register(
    SRCS input.c main.c params.c
    PRIV_REQUIRES esp_driver_uart
    INCLUDE_DIRS "")
