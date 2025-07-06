#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_err.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "image.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define LCD_HOST SPI2_HOST
#define LCD_H_RES (320)
#define LCD_V_RES (240)
#define SCLK_HZ (20 * 1000 * 1000)

#if CONFIG_IDF_TARGET_ESP32C3
#define GPIO_LCD_SCLK (GPIO_NUM_2)
#define GPIO_LCD_MOSI (GPIO_NUM_3)
#define GPIO_LCD_DC (GPIO_NUM_0)
#define GPIO_LCD_RST (GPIO_NUM_1)
#define GPIO_LCD_CS (GPIO_NUM_10)
#elif CONFIG_IDF_TARGET_ESP32S3
#define GPIO_LCD_SCLK (GPIO_NUM_4)
#define GPIO_LCD_MOSI (GPIO_NUM_5)
#define GPIO_LCD_DC (GPIO_NUM_6)
#define GPIO_LCD_RST (GPIO_NUM_7)
#define GPIO_LCD_CS (GPIO_NUM_15)
#else
#error "Not supported"
#endif

#define DELAY_TIME_MS (1000)

static const char *TAG = "esp_display";

static SemaphoreHandle_t trans_done = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t *color_buffer = NULL;

IRAM_ATTR static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(trans_done, &need_yield);
    return (need_yield == pdTRUE);
}

static void initialize_ili9341()
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t bus_config = {
        .sclk_io_num = GPIO_LCD_SCLK,
        .mosi_io_num = GPIO_LCD_MOSI,
        .miso_io_num = -1,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_LCD_CS,
        .dc_gpio_num = GPIO_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = SCLK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ILI9341 panel driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_LCD_RST,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
#else
        .rgb_endian = LCD_RGB_ENDIAN_BGR, // Some ili9341s have RGB = 0, others RGB = 1. Mine is the latter
#endif
        .bits_per_pixel = sizeof(uint16_t) * 8};
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    ESP_ERROR_CHECK(esp_lcd_panel_disp_off(panel_handle, false));
#else
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
#endif
}

static void finalize_ili9341()
{
    ESP_ERROR_CHECK(esp_lcd_panel_del(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_io_del(io_handle));
    ESP_ERROR_CHECK(spi_bus_free(LCD_HOST));
}

void app_main(void)
{
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    trans_done = xSemaphoreCreateBinary();
    assert(trans_done != NULL);

    color_buffer = (uint16_t *)image_bin;
    assert(image_len == (LCD_H_RES * LCD_V_RES * sizeof(uint16_t)));

    initialize_ili9341();

    char c;
    int mirror_x = 0, mirror_y = 0;

    while (1)
    {
        printf("Choose a command [d=draw, x=mirror x, y=mirror y, q=quit]: ");
        c = (char)getchar();
        printf("%c\n", c);

        switch (c)
        {
        case 'd':
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle,
                                                      0, 0,
                                                      LCD_H_RES, LCD_V_RES,
                                                      color_buffer));
            xSemaphoreTake(trans_done, portMAX_DELAY);
            break;
        case 'x':
            mirror_x = !mirror_x;
            ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, mirror_y, mirror_x));
            break;
        case 'y':
            mirror_y = !mirror_y;
            ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, mirror_y, mirror_x));
            break;
        case 'q':
            goto __exit;
        }
    }

__exit:
    vSemaphoreDelete(trans_done);
    finalize_ili9341();
}
