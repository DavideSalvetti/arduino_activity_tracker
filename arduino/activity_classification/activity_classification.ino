#define CIRCULAR_BUFFER_INT_SAFE

#include <arduino_activity_classification_inferencing.h>
#include <Arduino_LSM9DS1.h>
//#include <C:\Users\Davide\Documents\Arduino\libraries\CircularBuffer\CircularBuffer.h>
#include <C:\Users\Matteo\Documents\Arduino\libraries\CircularBuffer\CircularBuffer.h>
//#include <C:\Users\Martina\Documents\Arduino\libraries\CircularBuffer\CircularBuffer.h>
#include "ArduinoBLE.h"

static bool debug_nn = false;
static uint16_t run_inference_every_ms = 5000;
uint16_t buffer_step = 0;
unsigned long timetowait;
osThreadId_t main_thread_id;

uint8_t classified_id;
bool send_BLE = false;


static float inference_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
CircularBuffer<float, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE> bufferCircolare;

BLEService predictionService("e2e65ffc-5687-4cbe-8f2d-db76265f269f");
BLEUnsignedCharCharacteristic predictionCharacteristic("3000", BLERead | BLENotify);
BLEDevice central;

static rtos::Thread dataread_thread(osPriorityRealtime);
static rtos::Thread BLE_thread(osPriorityNormal);

void run_inference_background();
void get_IMU_data();
void updateBLE();

void setup() {
  Serial.begin(115200);

  delay(5000);

  if (!IMU.begin()) {
    ei_printf("Failed to initialize IMU!\r\n");
  } else {
    ei_printf("IMU initialized\r\n");
  }
  if (EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME != 5) {
    ei_printf("ERR: EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME should be equal to 5 (3 acc + 2 gyro)\n");
    return;
  }
  dataread_thread.start(mbed::callback(&get_IMU_data));
  BLE_thread.start(mbed::callback(&updateBLE));

  if(!BLE.begin()){
    while(true);
  }

  BLE.setLocalName("Activity Classificator");
  BLE.setAdvertisedService(predictionService);
  predictionService.addCharacteristic(predictionCharacteristic);
  BLE.addService(predictionService);
  BLE.advertise();

  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  main_thread_id = osThreadGetId();
    // wait until we have a full buffer
  rtos::Thread::signal_wait(0x1);

  // This is a structure that smoothens the output result
  // With the default settings 70% of readings should be the same before classifying.
  ei_classifier_smooth_t smooth;
  ei_classifier_smooth_init(&smooth, 10 /* no. of readings */, 7 /* min. readings the same */, 0.8 /* min. confidence */, 0.3 /* max anomaly */);

  while (1) {
    for(unsigned short i = 0; i < bufferCircolare.size(); i++){
      inference_buffer[i] = bufferCircolare[i];
    }

    // Turn the raw buffer in a signal which we can the classify
    signal_t signal;
    int err = numpy::signal_from_buffer(inference_buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    if (err != 0) {
      ei_printf("Failed to create signal from buffer (%d)\n", err);
      return;
    }

    // Run the classifier
    ei_impulse_result_t result = { 0 };

    err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
      ei_printf("ERR: Failed to run classifier (%d)\n", err);
      return;
    }
    // print the predictions
    ei_printf("Predictions ");
    ei_printf("(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
              result.timing.dsp, result.timing.classification, result.timing.anomaly);
    ei_printf(": ");

    const char* prediction = ei_classifier_smooth_update(&smooth, &result);
    uint8_t prediction_int = ei_classifier_smooth_update_int(&smooth, &result);
    classified_id = prediction_int;
    send_BLE = true;

    ei_printf("%s ", prediction);
    ei_printf("%d ", prediction_int);
    // print the cumulative results
    ei_printf(" [ ");
    for (size_t ix = 0; ix < smooth.count_size; ix++) {
      ei_printf("%u", smooth.count[ix]);
      if (ix != smooth.count_size + 1) {
        ei_printf(", ");
      } else {
        ei_printf(" ");
      }
    }
    ei_printf("]\n");
   
    rtos::Thread::wait(run_inference_every_ms);
  }

  ei_classifier_smooth_free(&smooth);
}
void get_IMU_data() {
  float accx, accy, accz, gyrox, gyroy, gyroz;
  while (1) {
    timetowait = millis();

    if (IMU.accelerationAvailable()) {
      IMU.readAcceleration(accx,accy,accz);
    } 
    if (IMU.gyroscopeAvailable()) {
       IMU.readGyroscope(gyrox,gyroy,gyroz);
    }

    bufferCircolare.push(accx);
    bufferCircolare.push(accy);
    bufferCircolare.push(accz);
    bufferCircolare.push(gyroy);
    bufferCircolare.push(gyroz);

    if (buffer_step < EI_CLASSIFIER_RAW_SAMPLE_COUNT - 1) {
      buffer_step++;
    } else if (buffer_step != 2000) {
      buffer_step++;
      osSignalSet(main_thread_id, 0x1);
    }

    timetowait = millis() - timetowait;
    rtos::Thread::wait(EI_CLASSIFIER_INTERVAL_MS-timetowait);
  }
}

void updateBLE(){
  while(1){
    central = BLE.central();

    if(central && send_BLE){
      digitalWrite(LED_BUILTIN, HIGH);
      predictionCharacteristic.writeValue(classified_id);
      send_BLE = false;
    } else{
      digitalWrite(LED_BUILTIN, LOW);
    }
    rtos::Thread::wait(10);
  }
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_FUSION
#error "Invalid model for current sensor"
#endif
