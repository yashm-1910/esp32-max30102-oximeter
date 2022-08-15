#include <Wire.h>
#include <SPI.h>
#include "algorithm_by_RF.h"
#include "max30102.h"
#include "MAX30105.h"                                     // MAX3010x library
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>

TaskHandle_t Task1;
TaskHandle_t Task2;

const char* ssid = "TP-LINK_5112";
const char* password = "30100305";

// Domain Name with full URL Path for HTTP POST Request
const char* serverName = "http://192.168.0.113/update";

// Interrupt pin
const byte oxiInt = 15;                                   // pin connected to MAX30102 INT
MAX30105 particleSensor;

uint32_t aun_ir_buffer[BUFFER_SIZE];                      //infrared LED sensor data
uint32_t aun_red_buffer[BUFFER_SIZE];                     //red LED sensor data
float old_n_spo2;                                         // Previous SPO2 value
uint8_t uch_dummy, k;
bool isFingerPlaced = false;

LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {

  pinMode(oxiInt, INPUT);                                 //pin 15 connects to the interrupt output pin of the MAX30102

  Wire.begin();

  particleSensor.begin(Wire, I2C_SPEED_STANDARD);         // Use default I2C port, 400kHz speed
  particleSensor.setup();

  Serial.begin(115200);

  WiFi.begin(ssid, password);
  Serial.println("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to WiFi network with IP Address: ");
  Serial.println(WiFi.localIP());

  lcd.init();
  lcd.backlight();

  maxim_max30102_reset();                                 //resets the MAX30102
  delay(1000);
  maxim_max30102_read_reg(REG_INTR_STATUS_1, &uch_dummy); //Reads/clears the interrupt status register
  maxim_max30102_init();                                  //initialize the MAX30102
  old_n_spo2 = 0.0;

  uint8_t revID;
  uint8_t partID;

  maxim_max30102_read_reg(0xFE, &revID);
  maxim_max30102_read_reg(0xFF, &partID);

  xTaskCreatePinnedToCore(
    Task1code,   /* Task function. */
    "Task1",     /* name of task. */
    10000,       /* Stack size of task */
    NULL,        /* parameter of the task */
    1,           /* priority of the task */
    &Task1,      /* Task handle to keep track of created task */
    0);          /* pin task to core 0 */
  delay(500);

  //create a task that will be executed in the Task2code() function, with priority 1 and executed on core 1
  xTaskCreatePinnedToCore(
    Task2code,   /* Task function. */
    "Task2",     /* name of task. */
    10000,       /* Stack size of task */
    NULL,        /* parameter of the task */
    1,           /* priority of the task */
    &Task2,      /* Task handle to keep track of created task */
    1);          /* pin task to core 1 */
  delay(500);


#ifdef DEBUG
  Serial.print("Rev ");
  Serial.print(revID, HEX);
  Serial.print(" Part ");
  Serial.println(partID, HEX);
  Serial.print("-------------------------------------");
#endif

}

float n_spo2;                                         //sop2 value
int32_t n_heart_rate;                                 //heart rate value

//Task1code: Continuously taking samples from MAX30102.  
void Task1code( void * pvParameters ) {
  Serial.print("Task1 running on core ");
  Serial.println(xPortGetCoreID());

  for (;;) {
    processHRandSPO2();
  }
}

//Task2code: send data
void Task2code( void * pvParameters ) {
  Serial.print("Task2 running on core ");
  Serial.println(xPortGetCoreID());

  for (;;) {
    sendDataToServer(n_heart_rate, n_spo2, isFingerPlaced);
  }
}

void loop() {}

void setDisplay(int32_t hr, float spo2) {
  if (hr == -999) {
    hr = 0;
    spo2 = 0;
  }
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("PRbpm: ");
  lcd.print(hr);

  lcd.setCursor(0, 1);
  lcd.print("SpO2: ");
  lcd.print(spo2);
  lcd.print("%");



  Serial.println("--RF--");

  Serial.print("\tSpO2: ");
  Serial.print(spo2);

  Serial.print("\tHR: ");
  Serial.print(hr, DEC);
  Serial.print("\n");

}

void fingerRemoved() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Place finger");
  lcd.setCursor(0, 1);
  lcd.print("on sensor");
}

void processHRandSPO2() {
  long irValue = particleSensor.getIR();                  // Reading the IR value it will permit us to know if there's a finger on the sensor or not
  if (irValue > 50000) {
    if (isFingerPlaced == false) {
      isFingerPlaced = true;
      setDisplay(-999, -999);
    }
    float ratio, correl;
    int8_t ch_spo2_valid;                                 //indicator to show if the SPO2 calculation is valid

    int8_t  ch_hr_valid;                                  //indicator to show if the heart rate calculation is valid
    int32_t i;
    char hr_str[10];

    //buffer length of BUFFER_SIZE stores ST seconds of samples running at FS sps
    //read BUFFER_SIZE samples, and determine the signal range
    for (i = 0; i < BUFFER_SIZE; i++)
    {
      while (digitalRead(oxiInt) == 1) {                  //wait until the interrupt pin asserts
        yield();
      }


      //IMPORTANT:
      //IR and LED are swapped here for MH-ET MAX30102. Check your vendor for MAX30102
      //and use this or the commented-out function call.
      maxim_max30102_read_fifo((aun_ir_buffer + i), (aun_red_buffer + i));
      //maxim_max30102_read_fifo((aun_red_buffer+i), (aun_ir_buffer+i));
    }

    //calculate heart rate and SpO2 after BUFFER_SIZE samples (ST seconds of samples) using Robert's method
    rf_heart_rate_and_oxygen_saturation(aun_ir_buffer, BUFFER_SIZE, aun_red_buffer, &n_spo2, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid, &ratio, &correl);
    setDisplay(n_heart_rate, n_spo2);


  } else {

    isFingerPlaced = false;
    Serial.println("No finger");

    fingerRemoved();
  }
}

void sendDataToServer(int heartRate, float spO2, bool fingerPlaced) {

  if (heartRate == -999) {
    heartRate = 0;
    spO2 = 0;
  }
  WiFiClient client;
  HTTPClient http;
  String httpSend = "?heartRate=" + String(heartRate) + "&spO2=" + String(spO2) + "&fingerPlaced=" + String(fingerPlaced);

  // Your Domain name with URL path or IP address with path
  http.begin(client, (serverName + httpSend));

  // Specify content-type header
  http.addHeader("Content-Type", "text/plain");

  // Send HTTP GET request
  int httpResponseCode = http.GET();

  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);

  // Free resources
  http.end();
  delay(2000);
}
