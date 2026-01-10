#include "sdkconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

static const char *TAG = "ir_capture";

#ifndef CONFIG_IR_RX_GPIO
#define CONFIG_IR_RX_GPIO 4
#endif

#define UART_PORT           UART_NUM_0
#define UART_BUF_SIZE       256
#define RMT_RX_CHANNEL      RMT_CHANNEL_0
#define MAX_CAPTURE_ITEMS   1024

static RingbufHandle_t s_ringbuf_handle = NULL;
static rmt_item32_t s_capture_buffer[MAX_CAPTURE_ITEMS];
static size_t s_capture_count = 0;
static size_t s_capture_head = 0;

static void push_capture_item(const rmt_item32_t *item) {
	if (MAX_CAPTURE_ITEMS == 0) {
		return;
	}
	s_capture_buffer[s_capture_head] = *item;
	s_capture_head = (s_capture_head + 1) % MAX_CAPTURE_ITEMS;
	if (s_capture_count < MAX_CAPTURE_ITEMS) {
		s_capture_count++;
	}
}

static void init_uart(void) {
	const uart_config_t uart_cfg = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
	ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
								 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void init_rmt(void) {
	rmt_config_t rx_config = {
		.rmt_mode = RMT_MODE_RX,
		.channel = RMT_RX_CHANNEL,
		.gpio_num = CONFIG_IR_RX_GPIO,
		.clk_div = 80,
		.mem_block_num = 4,
		.rx_config.filter_en = true,
		.rx_config.filter_ticks_thresh = 100,
		.rx_config.idle_threshold = 15000,
	};

	ESP_ERROR_CHECK(rmt_config(&rx_config));
	ESP_ERROR_CHECK(rmt_driver_install(RMT_RX_CHANNEL, 2000, 0));
	ESP_ERROR_CHECK(rmt_get_ringbuf_handle(RMT_RX_CHANNEL, &s_ringbuf_handle));
	ESP_ERROR_CHECK(rmt_rx_start(RMT_RX_CHANNEL, true));
	ESP_ERROR_CHECK(rmt_rx_stop(RMT_RX_CHANNEL));
}

static int read_duration_ms_from_uart(void) {
	char buffer[16] = {0};
	size_t idx = 0;

	while (idx < sizeof(buffer) - 1) {
		uint8_t ch = 0;
		const int len = uart_read_bytes(UART_PORT, &ch, 1, portMAX_DELAY);
		if (len <= 0) {
			continue;
		}

		if (ch == '\r') {
			continue;
		}
		if (ch == '\n') {
			break;
		}
		if (!isdigit((int)ch)) {
			idx = 0;
			memset(buffer, 0, sizeof(buffer));
			uart_write_bytes(UART_PORT, "Invalid input, enter milliseconds.\r\n", 39);
			continue;
		}

		buffer[idx++] = (char)ch;
	}

	return atoi(buffer);
}

static void capture_for_duration(uint32_t duration_ms) {
	s_capture_count = 0;
	s_capture_head = 0;
	ESP_ERROR_CHECK(rmt_rx_start(RMT_RX_CHANNEL, true));
	vTaskDelay(pdMS_TO_TICKS(duration_ms));
	ESP_ERROR_CHECK(rmt_rx_stop(RMT_RX_CHANNEL));

	while (true) {
		size_t item_size = 0;
		rmt_item32_t *items = (rmt_item32_t *)xRingbufferReceive(s_ringbuf_handle, &item_size,
																  pdMS_TO_TICKS(10));
		if (!items) {
			break;
		}

		size_t item_count = item_size / sizeof(rmt_item32_t);
		for (size_t i = 0; i < item_count; ++i) {
			push_capture_item(&items[i]);
		}
		vRingbufferReturnItem(s_ringbuf_handle, (void *)items);
	}
}

static void dump_capture_to_uart(void) {
	char line[96];
	if (s_capture_count == 0) {
		uart_write_bytes(UART_PORT, "END\r\n", 5);
		return;
	}

	size_t start_idx = (s_capture_head + MAX_CAPTURE_ITEMS - s_capture_count) % MAX_CAPTURE_ITEMS;
	for (size_t i = 0; i < s_capture_count; ++i) {
		size_t idx = (start_idx + i) % MAX_CAPTURE_ITEMS;
		const rmt_item32_t *item = &s_capture_buffer[idx];
		int len = snprintf(line, sizeof(line),
						   "%u,%u,%u,%u,%u\r\n",
				   (unsigned)i,
						   (unsigned)item->level0, (unsigned)item->duration0,
						   (unsigned)item->level1, (unsigned)item->duration1);
		uart_write_bytes(UART_PORT, line, len);
	}
	uart_write_bytes(UART_PORT, "END\r\n", 5);
}

void app_main(void) {
	init_uart();
	init_rmt();

	while (true) {
		const char *prompt = "\r\nEnter capture duration in ms: ";
		uart_write_bytes(UART_PORT, prompt, strlen(prompt));
		int duration = read_duration_ms_from_uart();
		if (duration <= 0) {
			uart_write_bytes(UART_PORT, "Duration must be > 0\r\n", 24);
			continue;
		}

		ESP_LOGI(TAG, "Capturing for %d ms", duration);
		capture_for_duration((uint32_t)duration);
		ESP_LOGI(TAG, "Captured %u items", (unsigned)s_capture_count);
		dump_capture_to_uart();
	}
}
