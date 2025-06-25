#include "sdkconfig.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "input.h"
#include "params.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "portmacro.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// -----------------------------------------------------------------------------
// LUT with approximate uint32 e^x for x=-128..127
static const uint32_t g_exp_lut_32[256] = {
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000001,
    0x00000002,
    0x00000005,
    0x0000000D,
    0x00000023,
    0x0000005E,
    0x00000100,
    0x000002B8,
    0x00000764,
    0x00001416,
    0x00003699,
    0x0000946A,
    0x0001936E,
    0x000448A2,
    0x000BA4F5,
    0x001FA715,
    0x00560A77,
    0x00E9E224,
    0x027BC2CB,
    0x06C02D64,
    0x1259AC49,
    0x31E1995F,
    0x87975E85,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
    0xFFFFFFFF,
};

// -----------------------------------------------------------------------------
// saturate a 32-bit integer to int8 range [INT8_MIN..INT8_MAX]
static inline int8_t saturate_to_int8(int32_t x)
{
    if (x > INT8_MAX)
    {
        return INT8_MAX;
    }
    else if (x < INT8_MIN)
    {
        return INT8_MIN;
    }
    else
    {
        return (int8_t)x;
    }
}

// -----------------------------------------------------------------------------
// saturating rounding doubling high mul: (a * b + 2^30) >> 31
static inline int32_t saturating_rounding_doubling_high_mul(int32_t a, int32_t b)
{
    // special case: INT32_MIN * INT32_MIN => saturates
    if (a == INT32_MIN && b == INT32_MIN)
    {
        return INT32_MAX;
    }

    // 32-bit only version of: (int64_t)a * (int64_t)b
    int32_t a_high = a >> 16;
    int32_t a_low = a & 0xffff;
    int32_t b_high = b >> 16;
    int32_t b_low = b & 0xffff;

    int32_t high_high = a_high * b_high;
    int32_t high_low = a_high * b_low;
    int32_t low_high = a_low * b_high;
    int32_t low_low = a_low * b_low;

    // 32x32 = 64 approximation: extract top 32 bits of result
    // combine partial products and simulate rounding
    int32_t mid = (low_high + high_low);
    int32_t mid_high = mid >> 15;
    int32_t result = high_high << 1;
    result += mid_high;
    result += (low_low >> 31); // rounding

    return result;
}

// -----------------------------------------------------------------------------
// rounding right shift by power-of-two
static inline int32_t rounding_divide_by_pot(int32_t x, int32_t exponent)
{
    int32_t mask = (1 << exponent) - 1;
    int32_t remainder = x & mask;
    int32_t threshold = (mask >> 1) + ((x < 0) ? 1 : 0);
    return (x >> exponent) + (remainder > threshold ? 1 : 0);
}

// -----------------------------------------------------------------------------
// multiply by quantized multiplier
static inline int32_t multiply_by_quantized_multiplier(int32_t val, uint32_t multiplier, int32_t shift)
{
    int32_t result = saturating_rounding_doubling_high_mul(val, (int32_t)multiplier);
    return rounding_divide_by_pot(result, shift);
}

// -----------------------------------------------------------------------------
// ReLU on int8
static inline int8_t relu_int8(int8_t x, int8_t zero_point)
{
    if (x < zero_point)
    {
        return zero_point;
    }
    else
    {
        return x;
    }
}

// -----------------------------------------------------------------------------
// dense (int8)
static void dense_int8(
    const int8_t *inputs,
    int8_t input_zp,
    const int8_t *weights,
    const int8_t *weight_zps,
    const int32_t *biases,
    int8_t *outputs,
    int8_t output_zp,
    const uint32_t *multipliers,
    const int32_t *shifts,
    uint32_t input_size,
    uint32_t output_size)
{
    for (uint32_t oc = 0; oc < output_size; ++oc)
    {
        // accumulate in 32-bit
        int32_t acc = 0;

        // weighted sum
        for (uint32_t ic = 0, wc = oc * input_size; ic < input_size; ++ic, ++wc)
        {
            int32_t x = (int32_t)inputs[ic] - (int32_t)input_zp;
            int32_t w = (int32_t)weights[wc] - (int32_t)weight_zps[oc];
            acc += x * w;
        }

        acc += biases[oc];

        acc = multiply_by_quantized_multiplier(acc, multipliers[oc], shifts[oc]);

        acc += (int32_t)output_zp;

        outputs[oc] = saturate_to_int8(acc);
    }
}

// -----------------------------------------------------------------------------
// dense + ReLU (int8)
static void dense_relu_int8(
    const int8_t *inputs,
    int8_t input_zp,
    const int8_t *weights,
    const int8_t *weight_zps,
    const int32_t *biases,
    int8_t *outputs,
    int8_t output_zp,
    const uint32_t *multipliers,
    const int32_t *shifts,
    uint32_t input_size,
    uint32_t output_size)
{
    dense_int8(
        inputs,
        input_zp,
        weights,
        weight_zps,
        biases,
        outputs,
        output_zp,
        multipliers,
        shifts,
        input_size,
        output_size);

    for (uint32_t oc = 0; oc < output_size; ++oc)
    {
        outputs[oc] = relu_int8(outputs[oc], output_zp);
    }
}

// -----------------------------------------------------------------------------
// in-place approximate softmax (int8)
//   1) find max logit
//   2) shift each logit by (logit[i] - max_logit)
//   3) convert to e^(shifted_value) using LUT
//   4) accumulate sum
//   5) out[i] = (127 * e^shifted_value) / sum (clamped to int8)
static void softmax_int8_inplace(int8_t *data, uint32_t length)
{
    // 1) find max
    int8_t max_val = data[0];
    for (uint32_t i = 1; i < length; ++i)
    {
        if (data[i] > max_val)
        {
            max_val = data[i];
        }
    }

    // 2) exponential transform
    // store intermediate e^x in a 32-bit
    // sum_exp in 32-bit as well
    // clamp partial sums to avoid overflow
    uint32_t exp_array[16]; // arbitrary max length
    uint32_t sum_exp = 0;

    for (uint32_t i = 0; i < length; ++i)
    {
        int32_t shift_val = (int32_t)data[i] - (int32_t)max_val; // range -255..255
        if (shift_val < -128)
        {
            shift_val = -128; // clamp
        }
        else if (shift_val > 127)
        {
            shift_val = 127; // clamp
        }

        // look up e^shift_val from the LUT [0..255]
        uint32_t e_val = g_exp_lut_32[shift_val + 128];

        exp_array[i] = e_val;

        // accumulte
        sum_exp += e_val;

        if (sum_exp < e_val)
        {
            // clamp sum_exp to max 32-bit
            sum_exp = UINT32_MAX;
        }
    }

    // 3) out[i] = (127 * exp_array[i]) / sum_exp
    for (uint32_t i = 0; i < length; ++i)
    {
        if (sum_exp == 0)
        {
            data[i] = 0;
        }
        else
        {
            uint32_t val = (exp_array[i] * 127) / sum_exp;
            if (val > 127)
            {
                val = 127;
            }
            data[i] = (int8_t)val;
        }
    }
}

// -----------------------------------------------------------------------------
// forward pass:
//   1) dense+ReLU
//   2) dense
//   3) softmax
static void forward_pass(const int8_t *inputs, int8_t *outputs)
{
    int8_t hiddens[HIDDEN_SIZE];

    const int8_t *hidden_weights = (int8_t *)&g_params[HIDDEN_WEIGHT_OFFSET];
    const int32_t *hidden_biases = (int32_t *)&g_params[HIDDEN_BIAS_OFFSET];
    const int8_t *output_weights = (int8_t *)&g_params[OUTPUT_WEIGHT_OFFSET];
    const int32_t *output_biases = (int32_t *)&g_params[OUTPUT_BIAS_OFFSET];
    int8_t input_zp = (int8_t)g_params[INPUT_ZP_OFFSET];
    const int8_t *hidden_weight_zps = (int8_t *)&g_params[HIDDEN_WEIGHT_ZP_OFFSET];
    int8_t hidden_zp = (int8_t)g_params[HIDDEN_ZP_OFFSET];
    const uint32_t *layer1_multipliers = (uint32_t *)&g_params[LAYER1_MULTIPLIER_OFFSET];
    const int32_t *layer1_scales = (int32_t *)&g_params[LAYER1_SCALE_OFFSET];
    const int8_t *output_weight_zps = (int8_t *)&g_params[OUTPUT_WEIGHT_ZP_OFFSET];
    int8_t output_zp = (int8_t)g_params[OUTPUT_ZP_OFFSET];
    const uint32_t *layer2_multipliers = (uint32_t *)&g_params[LAYER2_MULTIPLIER_OFFSET];
    const int32_t *layer2_scales = (int32_t *)&g_params[LAYER2_SCALE_OFFSET];

    // 1) dense+ReLU: input -> hidden
    dense_relu_int8(
        inputs,
        input_zp,
        hidden_weights,
        hidden_weight_zps,
        hidden_biases,
        hiddens,
        hidden_zp,
        layer1_multipliers,
        layer1_scales,
        INPUT_SIZE,
        HIDDEN_SIZE);

    // 2) dense (no activation) for final logits
    dense_int8(
        hiddens,
        hidden_zp,
        output_weights,
        output_weight_zps,
        output_biases,
        outputs,
        output_zp,
        layer2_multipliers,
        layer2_scales,
        HIDDEN_SIZE,
        OUTPUT_SIZE);

    // 3) in-place quantized softmax
    softmax_int8_inplace(outputs, OUTPUT_SIZE);
}

// -----------------------------------------------------------------------------
void app_main(void)
{
    int8_t inputs[INPUT_SIZE];
    int8_t outputs[OUTPUT_SIZE];
    char c;
    uint32_t start, end;

    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    printf("  __  __   _        _____    \n");
    printf(" |  \\/  | | |      |  __ \\ \n");
    printf(" | \\  / | | |      | |__) | \n");
    printf(" | |\\/| | | |      |  ___/  \n");
    printf(" | |  | | | |____  | |       \n");
    printf(" |_|  |_| |______| |_|       \n");
    printf("\n");

    while (1)
    {
        printf("Select digit [0-9]: ");

        c = (char)getchar();

        printf("%c\n", c);

        switch (c)
        {
        case '0':
            memcpy((void *)inputs, (void *)g_zero_input, g_input_len);
            break;
        case '1':
            memcpy((void *)inputs, (void *)g_one_input, g_input_len);
            break;
        case '2':
            memcpy((void *)inputs, (void *)g_two_input, g_input_len);
            break;
        case '3':
            memcpy((void *)inputs, (void *)g_three_input, g_input_len);
            break;
        case '4':
            memcpy((void *)inputs, (void *)g_four_input, g_input_len);
            break;
        case '5':
            memcpy((void *)inputs, (void *)g_five_input, g_input_len);
            break;
        case '6':
            memcpy((void *)inputs, (void *)g_six_input, g_input_len);
            break;
        case '7':
            memcpy((void *)inputs, (void *)g_seven_input, g_input_len);
            break;
        case '8':
            memcpy((void *)inputs, (void *)g_eight_input, g_input_len);
            break;
        case '9':
            memcpy((void *)inputs, (void *)g_nine_input, g_input_len);
            break;
        default:
            printf("Invalid digit: %c", c);
            continue;
        }

        static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&lock);
        start = esp_cpu_get_cycle_count();
        forward_pass(inputs, outputs);
        end = esp_cpu_get_cycle_count();
        portEXIT_CRITICAL(&lock);
        ESP_LOGI("esp_mlp", "Inference took %" PRIu32 " cycles", end - start);

        printf("******************************\n");
        for (uint32_t i = 0; i < OUTPUT_SIZE; ++i)
        {
            printf("Class %" PRIu32 " => %d\n", i, outputs[i]);
        }
        printf("******************************\n");
    }
}
