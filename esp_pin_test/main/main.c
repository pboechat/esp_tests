#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <stdlib.h>

#define TEST_COUNT 20
#define HALF_WAVE_DELAY 500
#define DEBUG_INPUT 0

struct WorkItem
{
    TaskHandle_t parent;
    gpio_num_t gpio_num;
};

void worker_task(void *arg)
{
    struct WorkItem *work_item = (struct WorkItem *)arg;

    gpio_reset_pin(work_item->gpio_num);
    gpio_hold_dis(work_item->gpio_num);
    gpio_sleep_sel_dis(work_item->gpio_num);
    gpio_deep_sleep_hold_dis();
#if DEBUG_INPUT
    gpio_set_direction(work_item->gpio_num, GPIO_MODE_INPUT_OUTPUT);
#endif
    gpio_set_direction(work_item->gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(work_item->gpio_num, GPIO_DRIVE_CAP_3);

    gpio_dump_io_configuration(stdout, BIT64(work_item->gpio_num));

    while (1)
    {
        gpio_set_level(work_item->gpio_num, 1);
#if DEBUG_INPUT
        printf("Current level: %d\n", gpio_get_level(work_item->gpio_num));
#endif
        vTaskDelay(pdMS_TO_TICKS(HALF_WAVE_DELAY));
        gpio_set_level(work_item->gpio_num, 0);
#if DEBUG_INPUT
        printf("Current level: %d\n", gpio_get_level(work_item->gpio_num));
#endif
        vTaskDelay(pdMS_TO_TICKS(HALF_WAVE_DELAY));

        if (ulTaskNotifyTake(pdTRUE, 0) == 1)
        {
            break;
        }
    }

    gpio_reset_pin(work_item->gpio_num);

    xTaskNotifyGive(work_item->parent);

    vTaskDelete(NULL);
}

void app_main(void)
{
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    printf("Square-wave pin test\n");

    struct WorkItem work_item;

    work_item.parent = xTaskGetCurrentTaskHandle();

    TaskHandle_t worker_handle = NULL;

    int pin_idx = 0;
    char c;
    while (1)
    {
        printf("Select pin: ");

        pin_idx = 0;
        while (1)
        {
            c = (char)getchar();
            if (c >= '0' && c <= '9')
            {
                pin_idx = (int)(c - '0') + (pin_idx * 10);
                printf("%c", c);
            }
            else if (c == '\n')
            {
                break;
            }
        }

        printf("\n");

        if (GPIO_IS_VALID_GPIO(pin_idx))
        {
            work_item.gpio_num = (gpio_num_t)pin_idx;

            xTaskCreate(worker_task, "worker", 4096, (void *)&work_item, 5, &worker_handle);

            printf("Press enter to end...\n");
            getchar();

            xTaskNotifyGive(worker_handle);

            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            worker_handle = NULL;
        }
        else
        {
            printf("Invalid pin selected\n");
        }
    }
}
