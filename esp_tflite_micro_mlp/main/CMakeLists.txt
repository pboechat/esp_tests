idf_component_register(
    SRCS input.cc main.cc model.cc
    PRIV_REQUIRES spi_flash esp_driver_uart
    INCLUDE_DIRS "")
