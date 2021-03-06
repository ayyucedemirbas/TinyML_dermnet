/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
  ==============================================================================*/
#include <TensorFlowLite.h>
#include "mbed.h"
#define SOUND_PIN 7
#include "main_functions.h"
#include "image_provider.h"
#include "model_settings.h"
#include "person_detect_model_data.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"
#include <Arduino_OV767X.h>

// Globals, used for compatibility with Arduino-style sketches.
namespace {
tflite::ErrorReporter* error_reporter = nullptr;
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;

// In order to use optimized tensorflow lite kernels, a signed int8_t quantized
// model is preferred over the legacy unsigned model format. This means that
// throughout this project, input images must be converted from unisgned to
// signed format. The easiest and quickest way to convert from unsigned to
// signed 8-bit integers is to subtract 128 from the unsigned value to get a
// signed value.

// An area of memory to use for input, output, and intermediate arrays.
constexpr int kTensorArenaSize = 136 * 1024;
static uint8_t tensor_arena[kTensorArenaSize];
}  // namespace

//needed for alarm code
boolean shouldAlarm = false;
int numPowerPoseClassifications = 0;
int lastMin = -1;
int lastHours = -1;


//how many frames you must hold the poes
const int numSamples = 8;

// The name of this function is important for Arduino compatibility.
void setup() {
  struct tm setTime;

  setTime.tm_mday = 30; //set day of month
  setTime.tm_mon = 6; //set month
  setTime.tm_year = 2021 - 1900; //set years since 1900
  setTime.tm_hour = 0; //set hours
  setTime.tm_min = 0; //set minutes
  setTime.tm_sec = 0; //set seconds

  set_time(mktime( &setTime ) );

  // Set up logging. Google style is to avoid globals or statics because of
  // lifetime uncertainty, but since this has a trivial destructor it's okay.
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroErrorReporter micro_error_reporter;
  error_reporter = &micro_error_reporter;

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  model = tflite::GetModel(g_person_detect_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    TF_LITE_REPORT_ERROR(error_reporter,
                         "Model provided is schema version %d not equal "
                         "to supported version %d.",
                         model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  //
  // tflite::AllOpsResolver resolver;
  // NOLINTNEXTLINE(runtime-global-variables)
  /*static tflite::MicroMutableOpResolver<5> micro_op_resolver;
  micro_op_resolver.AddConv2D();
  micro_op_resolver.AddMaxPool2D();
  micro_op_resolver.AddReshape();
  micro_op_resolver.AddSoftmax();
  micro_op_resolver.AddFullyConnected();*/

  static tflite::MicroMutableOpResolver<6> micro_op_resolver;
  micro_op_resolver.AddAveragePool2D();
  micro_op_resolver.AddConv2D();
  micro_op_resolver.AddDepthwiseConv2D();
  micro_op_resolver.AddReshape();
  micro_op_resolver.AddSoftmax();
  micro_op_resolver.AddFullyConnected();

  // Build an interpreter to run the model with.
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroInterpreter static_interpreter(
    model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
  interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    TF_LITE_REPORT_ERROR(error_reporter, "AllocateTensors() failed");
    return;
  }

  // Get information about the memory area to use for the model's input.
  input = interpreter->input(0);
  pinMode(SOUND_PIN, OUTPUT);
  //digitalWrite(SOUND_PIN, HIGH);
  Serial.print( "high" );
}

void loop() {

  
  time_t currTime = time(NULL);
  tm* currLocalTime = localtime(&currTime);
  int currHours = currLocalTime->tm_hour;
  int currMin = currLocalTime->tm_min;

  //print the system time
  Serial.print( "Time: " );
  Serial.print(currHours);
  Serial.print(":");
  Serial.print(currMin);
  Serial.print(":");
  Serial.println( currLocalTime->tm_sec );


  //count down till a 24 hour cycle, or we're on ourr first cycle.
  if ((lastHours == -1 || lastMin == -1) || (!shouldAlarm && lastHours == 23 && currHours == 0)) {
    //activate alarm
    shouldAlarm = true;
    Serial.print( "alarm true" );
  }

  lastHours = currHours;
  lastMin = currMin;


  if (true) {


    // Get image from provider.
    if (kTfLiteOk != GetImage(error_reporter, kNumCols, kNumRows, kNumChannels,
                              input->data.int8)) {
      TF_LITE_REPORT_ERROR(error_reporter, "Image capture failed.");
    }

    // Run the model on this input and make sure it succeeds.
    if (kTfLiteOk != interpreter->Invoke()) {
      TF_LITE_REPORT_ERROR(error_reporter, "Invoke failed.");
    }

    TfLiteTensor* output = interpreter->output(0);

    // Process the inference results.
    int8_t person_score = output->data.uint8[kPersonIndex];
    int8_t no_person_score = output->data.uint8[kNotAPersonIndex];
    for (int i = 0; i < kCategoryCount; i++) {
      int8_t curr_category_score = output->data.uint8[i];
      const char* currCategory = kCategoryLabels[i];
      TF_LITE_REPORT_ERROR(error_reporter, "%s : %d", currCategory, curr_category_score);
    }
    int8_t class1_score = output->data.uint8[0];
    int8_t class2_score = output->data.uint8[1];

    if (class1_score < class2_score) {
      numPowerPoseClassifications++;
    }
    digitalWrite(SOUND_PIN, LOW);
    Serial.print( "low" );

    //If we've captured numSamples pictures in the right pose, 
    //turn oof the alarm for another 24 hours. 
    if (numPowerPoseClassifications > numSamples) {
      digitalWrite(SOUND_PIN, HIGH);
      Serial.print( "high" );
      shouldAlarm = false;
      numPowerPoseClassifications = 0;
    }
  }
}
