#ifndef PARAMS_H_
#define PARAMS_H_

#include <inttypes.h>

// -----------------------------------------------------------------------------
// params

#define BATCH_SIZE                  1
#define INPUT_WIDTH                 28
#define INPUT_HEIGHT                28
#define INPUT_SIZE                  784
#define HIDDEN_SIZE                 128
#define OUTPUT_SIZE                 10

// -----------------------------------------------------------------------------
// hidden layer weights
#define HIDDEN_WEIGHT_OFFSET        0
#define HIDDEN_WEIGHT_SIZE          100352

// -----------------------------------------------------------------------------
// hidden layer biases
#define HIDDEN_BIAS_OFFSET          100352
#define HIDDEN_BIAS_SIZE            512

// -----------------------------------------------------------------------------
// output layer weights
#define OUTPUT_WEIGHT_OFFSET        100864
#define OUTPUT_WEIGHT_SIZE          1280

// -----------------------------------------------------------------------------
// output layer biases
#define OUTPUT_BIAS_OFFSET          102144
#define OUTPUT_BIAS_SIZE            40

// -----------------------------------------------------------------------------
// input layer zero-point
#define INPUT_ZP_OFFSET             102184
#define INPUT_ZP_SIZE               1

// -----------------------------------------------------------------------------
// hidden layer zero-point
#define HIDDEN_ZP_OFFSET            102185
#define HIDDEN_ZP_SIZE              1

// -----------------------------------------------------------------------------
// hidden layer weights zero-point
#define HIDDEN_WEIGHT_ZP_OFFSET     102186
#define HIDDEN_WEIGHT_ZP_SIZE       128

// -----------------------------------------------------------------------------
// layer1 multipler/shift
#define LAYER1_MULTIPLIER_OFFSET    102316
#define LAYER1_MULTIPLIER_SIZE      512
#define LAYER1_SCALE_OFFSET         102828
#define LAYER1_SCALE_SIZE           512

// -----------------------------------------------------------------------------
// output layer zero-point
#define OUTPUT_ZP_OFFSET            103341
#define OUTPUT_ZP_SIZE              1

// -----------------------------------------------------------------------------
// output layer weights zero-point
#define OUTPUT_WEIGHT_ZP_OFFSET     103342
#define OUTPUT_WEIGHT_ZP_SIZE       10

// -----------------------------------------------------------------------------
// layer2 multipler/shift
#define LAYER2_MULTIPLIER_OFFSET    103352
#define LAYER2_MULTIPLIER_SIZE      40
#define LAYER2_SCALE_OFFSET         103392
#define LAYER2_SCALE_SIZE           40

// -----------------------------------------------------------------------------
// total size
#define PARAMS_SIZE                 103432

extern const uint8_t g_params[];

#endif