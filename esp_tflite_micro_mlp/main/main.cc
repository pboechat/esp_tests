#include "sdkconfig.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "input.h"
#include "model.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/system_setup.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "portmacro.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

constexpr int TENSOR_ARENA_SIZE = 24 * 1024;

uint8_t g_tensor_arena[TENSOR_ARENA_SIZE];

static const char *get_TfLiteStatus_str(TfLiteStatus status)
{
    switch (status)
    {
    case TfLiteStatus::kTfLiteOk:
        return "TfLiteStatus::kTfLiteOk";
    case TfLiteStatus::kTfLiteError:
        return "TfLiteStatus::kTfLiteError";
    case TfLiteStatus::kTfLiteDelegateError:
        return "TfLiteStatus::kTfLiteDelegateError";
    case TfLiteStatus::kTfLiteApplicationError:
        return "TfLiteStatus::kTfLiteApplicationError";
    case TfLiteStatus::kTfLiteDelegateDataNotFound:
        return "TfLiteStatus::kTfLiteDelegateDataNotFound";
    case TfLiteStatus::kTfLiteDelegateDataWriteError:
        return "TfLiteStatus::kTfLiteDelegateDataWriteError";
    case TfLiteStatus::kTfLiteDelegateDataReadError:
        return "TfLiteStatus::kTfLiteDelegateDataReadError";
    case TfLiteStatus::kTfLiteUnresolvedOps:
        return "TfLiteStatus::kTfLiteUnresolvedOps";
    case TfLiteStatus::kTfLiteCancelled:
        return "TfLiteStatus::kTfLiteCancelled";
    case TfLiteStatus::kTfLiteOutputShapeNotKnown:
        return "TfLiteStatus::kTfLiteOutputShapeNotKnown";
    default:
        assert(false);
    }
}

static const char *get_TfLiteType_str(TfLiteType type)
{
    switch (type)
    {
    case TfLiteType::kTfLiteNoType:
        return "TfLiteType::kTfLiteNoType";
    case TfLiteType::kTfLiteFloat32:
        return "TfLiteType::kTfLiteFloat32";
    case TfLiteType::kTfLiteInt32:
        return "TfLiteType::kTfLiteInt32";
    case TfLiteType::kTfLiteUInt8:
        return "TfLiteType::kTfLiteUInt8";
    case TfLiteType::kTfLiteInt64:
        return "TfLiteType::kTfLiteInt64";
    case TfLiteType::kTfLiteString:
        return "TfLiteType::kTfLiteString";
    case TfLiteType::kTfLiteBool:
        return "TfLiteType::kTfLiteBool";
    case TfLiteType::kTfLiteInt16:
        return "TfLiteType::kTfLiteInt16";
    case TfLiteType::kTfLiteComplex64:
        return "TfLiteType::kTfLiteComplex64";
    case TfLiteType::kTfLiteInt8:
        return "TfLiteType::kTfLiteInt8";
    case TfLiteType::kTfLiteFloat16:
        return "TfLiteType::kTfLiteFloat16";
    case TfLiteType::kTfLiteFloat64:
        return "TfLiteType::kTfLiteFloat64";
    case TfLiteType::kTfLiteComplex128:
        return "TfLiteType::kTfLiteComplex128";
    case TfLiteType::kTfLiteUInt64:
        return "TfLiteType::kTfLiteUInt64";
    case TfLiteType::kTfLiteResource:
        return "TfLiteType::kTfLiteResource";
    case TfLiteType::kTfLiteVariant:
        return "TfLiteType::kTfLiteVariant";
    case TfLiteType::kTfLiteUInt32:
        return "TfLiteType::kTfLiteUInt32";
    case TfLiteType::kTfLiteUInt16:
        return "TfLiteType::kTfLiteUInt16";
    case TfLiteType::kTfLiteInt4:
        return "TfLiteType::kTfLiteInt4";
    case TfLiteType::kTfLiteBFloat16:
        return "TfLiteType::kTfLiteBFloat16";
    default:
        assert(false);
    }
}

void print_TfLiteTensor(const TfLiteTensor *tensor)
{
    assert(tensor != nullptr);
    MicroPrintf("TENSOR");
    MicroPrintf("    - type: %s", get_TfLiteType_str(tensor->type));
    MicroPrintf("    - bytes: %zu", tensor->bytes);
    char buffer[32];
    char *ptr = buffer;
    int j = 32;
    for (int i = 0; i < tensor->dims->size; ++i)
    {
        int c = MicroSnprintf(ptr, j, "%d ", tensor->dims->data[i]);
        ptr += c;
        j -= c;
    }
    *ptr = 0;
    MicroPrintf("    - dims: %s", buffer);
    MicroPrintf("    - data: ");
    for (size_t i = 0; i < tensor->bytes; ++i)
    {
        if ((i % 6) == 0)
        {
            if (i > 0)
            {
                *ptr = 0;
                MicroPrintf("%s", buffer);
            }
            ptr = buffer;
            j = 32;
        }
        int c = MicroSnprintf(ptr, j, "%4" PRIi8 " ", tensor->data.int8[i]);
        ptr += c;
        j -= c;
    }
    *ptr = 0;
    MicroPrintf("%s", buffer);
    MicroPrintf("\n");
}

extern "C" void app_main(void)
{
    uart_driver_install(uart_port_t(CONFIG_ESP_CONSOLE_UART_NUM), 256, 0, 0, nullptr, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    const tflite::Model *model = tflite::GetModel(g_model);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        MicroPrintf("Model provided is schema version %d not equal to supported version %d",
                    model->version(), TFLITE_SCHEMA_VERSION);
        return;
    }

    TfLiteStatus status;

    tflite::MicroMutableOpResolver<2> resolver;

    status = resolver.AddFullyConnected();
    if (status != kTfLiteOk)
    {
        MicroPrintf("tflite::MicroMutableOpResolver<>::AddFullyConnected() failed: %s", get_TfLiteStatus_str(status));
        return;
    }

    status = resolver.AddSoftmax();
    if (status != kTfLiteOk)
    {
        MicroPrintf("tflite::MicroMutableOpResolver<>::AddSoftmax() failed: %s", get_TfLiteStatus_str(status));
        return;
    }

    tflite::MicroInterpreter interpreter(
        model,
        resolver,
        g_tensor_arena,
        TENSOR_ARENA_SIZE);

    status = interpreter.AllocateTensors();
    if (status != kTfLiteOk)
    {
        MicroPrintf("tflite::MicroInterpreter::AllocateTensors() failed: %s", get_TfLiteStatus_str(status));
        return;
    }

    MicroPrintf("  __  __   _        _____    ");
    MicroPrintf(" |  \\/  | | |      |  __ \\ ");
    MicroPrintf(" | \\  / | | |      | |__) | ");
    MicroPrintf(" | |\\/| | | |      |  ___/  ");
    MicroPrintf(" | |  | | | |____  | |       ");
    MicroPrintf(" |_|  |_| |______| |_|       ");
    MicroPrintf("\n");

    // MicroPrintf("INPUTS: ");
    // for (size_t i = 0; i < interpreter.inputs()->size(); ++i)
    // {
    //     print_TfLiteTensor(interpreter.input(i));
    // }

    // MicroPrintf("OUTPUT: ");
    // for (size_t i = 0; i < interpreter.outputs()->size(); ++i)
    // {
    //     print_TfLiteTensor(interpreter.output(i));
    // }

    TfLiteTensor *input = interpreter.input(0);
    assert(input->bytes == g_input_len);

    TfLiteTensor *output = interpreter.output(0);
    assert(output->bytes == 10);

    while (true)
    {
        MicroPrintf("Select digit [0-9]: ");
        char c = (char)getchar();
        MicroPrintf("%c", c);
        switch (c)
        {
        case '0':
            memcpy((void *)input->data.raw, (void *)g_zero_input, g_input_len);
            break;
        case '1':
            memcpy((void *)input->data.raw, (void *)g_one_input, g_input_len);
            break;
        case '2':
            memcpy((void *)input->data.raw, (void *)g_two_input, g_input_len);
            break;
        case '3':
            memcpy((void *)input->data.raw, (void *)g_three_input, g_input_len);
            break;
        case '4':
            memcpy((void *)input->data.raw, (void *)g_four_input, g_input_len);
            break;
        case '5':
            memcpy((void *)input->data.raw, (void *)g_five_input, g_input_len);
            break;
        case '6':
            memcpy((void *)input->data.raw, (void *)g_six_input, g_input_len);
            break;
        case '7':
            memcpy((void *)input->data.raw, (void *)g_seven_input, g_input_len);
            break;
        case '8':
            memcpy((void *)input->data.raw, (void *)g_eight_input, g_input_len);
            break;
        case '9':
            memcpy((void *)input->data.raw, (void *)g_nine_input, g_input_len);
            break;
        default:
            MicroPrintf("Invalid digit: %c", c);
            continue;
        }

        static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&lock);
        uint32_t start = esp_cpu_get_cycle_count();
        status = interpreter.Invoke();
        uint32_t end = esp_cpu_get_cycle_count();
        portEXIT_CRITICAL(&lock);
        if (status != kTfLiteOk)
        {
            MicroPrintf("tflite::MicroInterpreter::Invoke() failed: %s", get_TfLiteStatus_str(status));
            return;
        }
        uint32_t cycles = end - start;
        ESP_LOGI("esp_mlp", "Inference took %" PRIu32 " cycles", cycles);

        MicroPrintf("******************************");
        for (uint32_t i = 0; i < 10; ++i)
        {
            MicroPrintf("Class %d => %d", i, output->data.int8[i]);
        }
        MicroPrintf("******************************");
    }
}
