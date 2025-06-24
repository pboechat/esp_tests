#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "led_strip.h"
#include "model.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cinttypes>
#include <cmath>

#if CONFIG_IDF_TARGET_ESP32C3
#define LED_GPIO 8
#elif CONFIG_IDF_TARGET_ESP32S3
#define LED_GPIO 38
#else
#error "Unsupported ESP-IDF target"
#endif

constexpr float kXrange = 2.f * 3.14159265359f;
constexpr int kInferencesPerCycle = 20;
constexpr int kTensorArenaSize = 2000;

static uint8_t tensor_arena[kTensorArenaSize];
static led_strip_handle_t led_strip;

/*
 * convert HSV triple to RGB triple
 * source: http://en.wikipedia.org/wiki/HSL_and_HSV#Converting_to_RGB
 */
static void hsv2rgb(uint8_t hsv[], uint8_t rgb[])
{
    uint16_t C;
    int16_t Hprime, Cscl;
    uint8_t hs, X, m;

    /* default */
    rgb[0] = 0;
    rgb[1] = 0;
    rgb[2] = 0;

    /* calcs are easy if v = 0 */
    if (hsv[2] == 0)
    {
        return;
    }

    /* C is the chroma component */
    C = ((uint16_t)hsv[1] * (uint16_t)hsv[2]) >> 8;

    /* Hprime is fixed point with range 0-5.99 representing hue sector */
    Hprime = (int16_t)hsv[0] * 6;

    /* get intermediate value X */
    Cscl = (Hprime % 512) - 256;
    Cscl = Cscl < 0 ? -Cscl : Cscl;
    Cscl = 256 - Cscl;
    X = ((uint16_t)C * Cscl) >> 8;

    /* m is value offset */
    m = hsv[2] - C;

    /* get the hue sector (1 of 6) */
    hs = (Hprime) >> 8;

    /* map by sector */
    switch (hs)
    {
    case 0:
        /* Red -> Yellow sector */
        rgb[0] = C + m;
        rgb[1] = X + m;
        rgb[2] = m;
        break;

    case 1:
        /* Yellow -> Green sector */
        rgb[0] = X + m;
        rgb[1] = C + m;
        rgb[2] = m;
        break;

    case 2:
        /* Green -> Cyan sector */
        rgb[0] = m;
        rgb[1] = C + m;
        rgb[2] = X + m;
        break;

    case 3:
        /* Cyan -> Blue sector */
        rgb[0] = m;
        rgb[1] = X + m;
        rgb[2] = C + m;
        break;

    case 4:
        /* Blue -> Magenta sector */
        rgb[0] = X + m;
        rgb[1] = m;
        rgb[2] = C + m;
        break;

    case 5:
        /* Magenta -> Red sector */
        rgb[0] = C + m;
        rgb[1] = m;
        rgb[2] = X + m;
        break;
    }
}

extern "C" void app_main(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {
            .invert_out = false,
        }};

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = (10 * 1000 * 1000), // 10 MHz
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        }};

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));

    const tflite::Model *model = tflite::GetModel(g_model);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        MicroPrintf("Model provided is schema version %d not equal to supported "
                    "version %d.",
                    model->version(), TFLITE_SCHEMA_VERSION);
        return;
    }

    tflite::MicroMutableOpResolver<1> resolver;
    if (resolver.AddFullyConnected() != kTfLiteOk)
    {
        return;
    }

    tflite::MicroInterpreter interpreter(model, resolver, tensor_arena, kTensorArenaSize);

    TfLiteStatus allocate_status = interpreter.AllocateTensors();
    if (allocate_status != kTfLiteOk)
    {
        MicroPrintf("AllocateTensors() failed");
        return;
    }

    TfLiteTensor *input = interpreter.input(0);
    TfLiteTensor *output = interpreter.output(0);

    int inference_count = 0;
    while (true)
    {
        float position = static_cast<float>(inference_count) /
                         static_cast<float>(kInferencesPerCycle);
        float x = position * kXrange;

        int8_t x_quantized = x / input->params.scale + input->params.zero_point;
        input->data.int8[0] = x_quantized;

        static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&lock);
        uint32_t start = esp_cpu_get_cycle_count();
        TfLiteStatus invoke_status = interpreter.Invoke();
        uint32_t end = esp_cpu_get_cycle_count();
        portEXIT_CRITICAL(&lock);
        uint32_t cycles = end - start;
        ESP_LOGI("tflite_micro_hello_world", "Inference took %" PRIu32 " cycles", cycles);

        if (invoke_status != kTfLiteOk)
        {
            MicroPrintf("Invoke failed on x: %f\n", static_cast<double>(x));
            return;
        }

        int8_t y_quantized = output->data.int8[0];
        float y = (y_quantized - output->params.zero_point) * output->params.scale;

        uint8_t hsv[3], rgb[3];
        hsv[0] = (uint8_t)std::min(std::max((int16_t)(std::floor(y * 128.0f) + 127.0f), int16_t(0)), int16_t(255));
        hsv[1] = 255;
        hsv[2] = 255;
        hsv2rgb(hsv, rgb);

        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, (uint32_t)rgb[0], (uint32_t)rgb[1], (uint32_t)rgb[2]));
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));

        // Log the current X and Y values
        MicroPrintf("x: %f, y: %f, rgb: [%d, %d, %d]", static_cast<double>(x),
                    static_cast<double>(y), rgb[0], rgb[1], rgb[2]);

        inference_count += 1;
        if (inference_count >= kInferencesPerCycle)
        {
            inference_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
