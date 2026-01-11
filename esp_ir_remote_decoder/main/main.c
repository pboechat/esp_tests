#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rmt_rx.h"
#include "driver/uart.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/soc_caps.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ir_remote_decoder";

#ifndef CONFIG_IR_RX_GPIO
#define CONFIG_IR_RX_GPIO 4
#endif

#ifndef CONFIG_IR_OLED_SDA_GPIO
#define CONFIG_IR_OLED_SDA_GPIO 8
#endif

#ifndef CONFIG_IR_OLED_SCL_GPIO
#define CONFIG_IR_OLED_SCL_GPIO 9
#endif

#define UART_PORT           		UART_NUM_0
#define UART_BUF_SIZE       		256
#define MAX_CAPTURE_ITEMS           1024
#define RMT_RESOLUTION_HZ           1000000
#define RMT_MEM_BLOCK_SYMBOLS       (SOC_RMT_MEM_WORDS_PER_CHANNEL * SOC_RMT_RX_CANDIDATES_PER_GROUP)
#define RMT_RAW_BUFFER_SYMBOLS      (4 * RMT_MEM_BLOCK_SYMBOLS)
#define RMT_RX_QUEUE_DEPTH          4
#define RMT_SIGNAL_RANGE_MIN_NS     1000
#define RMT_SIGNAL_RANGE_MAX_NS     30000000

#define OLED_I2C_PORT               I2C_NUM_0
#define OLED_I2C_SPEED_HZ           400000
#define OLED_SDA_GPIO               CONFIG_IR_OLED_SDA_GPIO
#define OLED_SCL_GPIO               CONFIG_IR_OLED_SCL_GPIO
#define OLED_I2C_ADDRESS            0x3C
#define OLED_WIDTH                  128
#define OLED_HEIGHT                 64
#define OLED_FRAMEBUFFER_BYTES      ((OLED_WIDTH * OLED_HEIGHT) / 8)
#define OLED_WAVE_HIGH_Y            18
#define OLED_WAVE_LOW_Y             46

static rmt_channel_handle_t s_rx_channel = NULL;
static QueueHandle_t s_rx_event_queue = NULL;
static rmt_symbol_word_t s_rx_raw_symbols[RMT_RAW_BUFFER_SYMBOLS];
static const rmt_receive_config_t s_rx_receive_config = {
	.signal_range_min_ns = RMT_SIGNAL_RANGE_MIN_NS,
	.signal_range_max_ns = RMT_SIGNAL_RANGE_MAX_NS,
	.flags = {
		.en_partial_rx = 1,
	},
};
static rmt_symbol_word_t s_capture_buffer[MAX_CAPTURE_ITEMS];
static size_t s_capture_count = 0;
static size_t s_capture_head = 0;
static bool s_oled_ready = false;
static uint8_t s_oled_framebuffer[OLED_FRAMEBUFFER_BYTES];
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_panel_io_handle_t s_oled_io = NULL;
static esp_lcd_panel_handle_t s_oled_panel = NULL;

static inline void oled_set_pixel(int x, int y, bool on) {
	if (!s_oled_ready) {
		return;
	}
	if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
		return;
	}
	size_t index = (size_t)x + ((size_t)(y >> 3) * OLED_WIDTH);
	uint8_t mask = (uint8_t)(1U << (y & 0x07));
	if (on) {
		s_oled_framebuffer[index] |= mask;
	} else {
		s_oled_framebuffer[index] &= (uint8_t)~mask;
	}
}

static void oled_draw_hline(int x0, int x1, int y) {
	if (x1 < x0) {
		int tmp = x0;
		x0 = x1;
		x1 = tmp;
	}
	if (x0 < 0) {
		x0 = 0;
	}
	if (x1 >= OLED_WIDTH) {
		x1 = OLED_WIDTH - 1;
	}
	for (int x = x0; x <= x1; ++x) {
		oled_set_pixel(x, y, true);
	}
}

static void oled_draw_vline(int x, int y0, int y1) {
	if (y1 < y0) {
		int tmp = y0;
		y0 = y1;
		y1 = tmp;
	}
	if (x < 0 || x >= OLED_WIDTH) {
		return;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (y1 >= OLED_HEIGHT) {
		y1 = OLED_HEIGHT - 1;
	}
	for (int y = y0; y <= y1; ++y) {
		oled_set_pixel(x, y, true);
	}
}

static void oled_flush_framebuffer(void) {
	if (!s_oled_ready || !s_oled_panel) {
		return;
	}
	esp_err_t err = esp_lcd_panel_draw_bitmap(s_oled_panel, 0, 0,
					OLED_WIDTH, OLED_HEIGHT, s_oled_framebuffer);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Failed to update OLED pixels: %s", esp_err_to_name(err));
	}
}

static void oled_plot_segment(uint64_t *cursor, uint64_t total_time,
				 uint32_t duration, uint32_t level, int *last_y) {
	if (duration == 0 || total_time == 0) {
		return;
	}
	int start_col = (int)((*cursor * (OLED_WIDTH - 1)) / total_time);
	int end_col = (int)(((*cursor + duration) * (OLED_WIDTH - 1)) / total_time);
	if (end_col < start_col) {
		end_col = start_col;
	}
	if (end_col >= OLED_WIDTH) {
		end_col = OLED_WIDTH - 1;
	}
	int y = level ? OLED_WAVE_HIGH_Y : OLED_WAVE_LOW_Y;
	if (*last_y >= 0 && *last_y != y) {
		oled_draw_vline(start_col, *last_y, y);
	}
	oled_draw_hline(start_col, end_col, y);
	*last_y = y;
	*cursor += duration;
}

static void oled_render_capture(void) {
	if (!s_oled_ready) {
		return;
	}
	memset(s_oled_framebuffer, 0, sizeof(s_oled_framebuffer));
	if (s_capture_count == 0) {
		oled_flush_framebuffer();
		return;
	}
	uint64_t total_time = 0;
	for (size_t i = 0; i < s_capture_count; ++i) {
		total_time += s_capture_buffer[i].duration0;
		total_time += s_capture_buffer[i].duration1;
	}
	if (total_time == 0) {
		total_time = 1;
	}
	uint64_t cursor = 0;
	int last_y = -1;
	for (size_t i = 0; i < s_capture_count; ++i) {
		oled_plot_segment(&cursor, total_time,
				s_capture_buffer[i].duration0,
				s_capture_buffer[i].level0,
				&last_y);
		oled_plot_segment(&cursor, total_time,
				s_capture_buffer[i].duration1,
				s_capture_buffer[i].level1,
				&last_y);
	}
	if (cursor < total_time) {
		oled_plot_segment(&cursor, total_time,
				(uint32_t)(total_time - cursor), 0, &last_y);
	}
	oled_flush_framebuffer();
}

static void init_oled(void) {
	if (s_oled_ready) {
		return;
	}
	if (!s_i2c_bus) {
		i2c_master_bus_config_t bus_cfg = {
			.i2c_port = OLED_I2C_PORT,
			.sda_io_num = OLED_SDA_GPIO,
			.scl_io_num = OLED_SCL_GPIO,
			.clk_source = I2C_CLK_SRC_DEFAULT,
			.glitch_ignore_cnt = 7,
			.flags = {
				.enable_internal_pullup = true,
			},
		};
		esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Failed to create I2C bus: %s", esp_err_to_name(err));
			return;
		}
	}
	if (!s_oled_io) {
		esp_lcd_panel_io_i2c_config_t io_cfg = {
			.dev_addr = OLED_I2C_ADDRESS,
			.control_phase_bytes = 1,
			.dc_bit_offset = 6,
			.lcd_cmd_bits = 8,
			.lcd_param_bits = 8,
			.scl_speed_hz = OLED_I2C_SPEED_HZ,
		};
		esp_err_t err = esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &s_oled_io);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Failed to create OLED panel IO: %s", esp_err_to_name(err));
			return;
		}
	}
	if (!s_oled_panel) {
		esp_lcd_panel_ssd1306_config_t vendor_cfg = {
			.height = OLED_HEIGHT,
		};
		esp_lcd_panel_dev_config_t panel_cfg = {
			.reset_gpio_num = -1,
			.color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
			.bits_per_pixel = 1,
			.vendor_config = &vendor_cfg,
		};
		esp_err_t err = esp_lcd_new_panel_ssd1306(s_oled_io, &panel_cfg, &s_oled_panel);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Failed to create OLED panel: %s", esp_err_to_name(err));
			return;
		}
		err = esp_lcd_panel_reset(s_oled_panel);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Failed to reset OLED panel: %s", esp_err_to_name(err));
			return;
		}
		err = esp_lcd_panel_init(s_oled_panel);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Failed to init OLED panel: %s", esp_err_to_name(err));
			return;
		}
		(void)esp_lcd_panel_mirror(s_oled_panel, true, true);
		err = esp_lcd_panel_disp_on_off(s_oled_panel, true);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Failed to enable OLED panel: %s", esp_err_to_name(err));
			return;
		}
	}
	memset(s_oled_framebuffer, 0, sizeof(s_oled_framebuffer));
	s_oled_ready = true;
	oled_flush_framebuffer();
}

static void push_capture_item(const rmt_symbol_word_t *item) {
	if (MAX_CAPTURE_ITEMS == 0) {
		return;
	}
	s_capture_buffer[s_capture_head] = *item;
	s_capture_head = (s_capture_head + 1) % MAX_CAPTURE_ITEMS;
	if (s_capture_count < MAX_CAPTURE_ITEMS) {
		s_capture_count++;
	}
}

static bool IRAM_ATTR rmt_rx_done_callback(rmt_channel_handle_t channel,
						   const rmt_rx_done_event_data_t *edata,
						   void *user_data) {
	(void)channel;
	QueueHandle_t queue = (QueueHandle_t)user_data;
	if (!queue || !edata) {
		return false;
	}
	BaseType_t high_task_wakeup = pdFALSE;
	rmt_rx_done_event_data_t event = *edata;
	(void)xQueueSendFromISR(queue, &event, &high_task_wakeup);
	return high_task_wakeup == pdTRUE;
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
	const rmt_rx_channel_config_t rx_config = {
		.gpio_num = (gpio_num_t)CONFIG_IR_RX_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RMT_RESOLUTION_HZ,
		.mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS,
		.intr_priority = 0,
		.flags = {
			.invert_in = 0,
			.with_dma = 0,
			.io_loop_back = 0,
			.allow_pd = 0,
		},
	};

	ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &s_rx_channel));
	s_rx_event_queue = xQueueCreate(RMT_RX_QUEUE_DEPTH, sizeof(rmt_rx_done_event_data_t));
	if (!s_rx_event_queue) {
		ESP_LOGE(TAG, "Failed to create RMT RX queue");
		abort();
	}

	const rmt_rx_event_callbacks_t callbacks = {
		.on_recv_done = rmt_rx_done_callback,
	};
	ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s_rx_channel, &callbacks, s_rx_event_queue));
	ESP_ERROR_CHECK(rmt_enable(s_rx_channel));
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

		if (ch == '\r' || ch == '\n') {
			if (idx == 0) {
				continue;
			}
			uart_write_bytes(UART_PORT, "\r\n", 2);
			break;
		}
		if (ch == 0x08 || ch == 0x7f) { /* handle backspace/delete */
			if (idx > 0) {
				idx--;
				buffer[idx] = '\0';
				uart_write_bytes(UART_PORT, "\b \b", 3);
			}
			continue;
		}
		if (!isdigit((int)ch)) {
			uart_write_bytes(UART_PORT, "\a", 1);
			continue;
		}

		buffer[idx++] = (char)ch;
		uart_write_bytes(UART_PORT, (const char *)&ch, 1);
	}

	return atoi(buffer);
}

static void capture_for_duration(uint32_t duration_ms) {
	if (!s_rx_channel || !s_rx_event_queue) {
		ESP_LOGE(TAG, "RMT RX channel not ready");
		return;
	}

	s_capture_count = 0;
	s_capture_head = 0;
	xQueueReset(s_rx_event_queue);

	ESP_ERROR_CHECK(rmt_receive(s_rx_channel, s_rx_raw_symbols, sizeof(s_rx_raw_symbols),
					 &s_rx_receive_config));
	bool keep_rxing = true;
	bool rx_inflight = true;
	uint32_t final_wait_count = 0;
	const uint32_t max_final_waits = 5;
	const TickType_t active_wait_ticks = pdMS_TO_TICKS(10);
	const TickType_t final_wait_ticks = pdMS_TO_TICKS(20);
	const int64_t capture_deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;

	while (keep_rxing || rx_inflight) {
		if (keep_rxing && esp_timer_get_time() >= capture_deadline) {
			keep_rxing = false;
		}

		rmt_rx_done_event_data_t event;
		const TickType_t wait_ticks = keep_rxing ? active_wait_ticks : final_wait_ticks;
		if (xQueueReceive(s_rx_event_queue, &event, wait_ticks) == pdTRUE) {
			rx_inflight = false;
			final_wait_count = 0;
			for (size_t i = 0; i < event.num_symbols; ++i) {
				push_capture_item(&event.received_symbols[i]);
			}
			if (keep_rxing) {
				ESP_ERROR_CHECK(rmt_receive(s_rx_channel, s_rx_raw_symbols,
							 sizeof(s_rx_raw_symbols), &s_rx_receive_config));
				rx_inflight = true;
			}
		} else if (!keep_rxing && rx_inflight) {
			if (++final_wait_count >= max_final_waits) {
				ESP_LOGW(TAG, "Timeout waiting for final RMT RX chunk");
				break;
			}
		} else if (!keep_rxing) {
			break;
		}
	}

	ESP_ERROR_CHECK(rmt_disable(s_rx_channel));
	ESP_ERROR_CHECK(rmt_enable(s_rx_channel));
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
		const rmt_symbol_word_t *item = &s_capture_buffer[idx];
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
	init_oled();

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
		oled_render_capture();
	}
}
