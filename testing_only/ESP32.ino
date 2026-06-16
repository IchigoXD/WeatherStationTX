#include <FS.h>
#include <LittleFS.h> 
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h> 

/* =========================================
 * PART 1: ESP32 LEAF & RAIN SENSOR SETTINGS
 * ========================================= */
const int PIN_PROBE      = 4;
const int PIN_RAIN_GAUGE = 14; // Connect tipping bucket between GPIO 14 and GND

const int PIN_POWER_LED  = 25; 
const int PIN_SENSOR_LED = 26; 
const int PIN_DATA_LED   = 27;

const int WINDOW_SIZE = 5; 
long historyBuffer[WINDOW_SIZE]; 
int historyIndex = 0;   
long latestLeafWetness = 0; 

// --- RAIN GAUGE VARIABLES ---
volatile uint16_t rainTips = 0;
volatile unsigned long lastTipTime = 0;
const unsigned long DEBOUNCE_TIME = 200; // 200ms debounce to prevent false triggers

const char* filename = "/data.csv";
unsigned long leafPreviousMillis = 0;
const long leafInterval = 1000; 

const int POWER_LED_BRIGHTNESS = 10;

/* =========================================
 * PART 2: TEMP SENSOR SETTINGS
 * ========================================= */
#define SENSOR_PIN_1 2
#define SENSOR_PIN_2 13
#define SENSOR_PIN_3 15
#define SENSOR_PIN_4 32
#define SENSOR_PIN_5 33

#define LED_READ_PIN 19
#define LED_START_PIN 23

// I2C SLAVE SETTINGS
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define SLAVE_ADDR 0x08  

#define MAX_SENSORS 5
#define ERROR_SENSOR_NOT_FOUND  -999.00
#define ERROR_OUT_OF_RANGE      -888.00
#define ERROR_NO_DATA           -777.00

int sensorPins[MAX_SENSORS] = {SENSOR_PIN_1, SENSOR_PIN_2, SENSOR_PIN_3, SENSOR_PIN_4, SENSOR_PIN_5};
OneWire* oneWire[MAX_SENSORS];
DallasTemperature* sensors[MAX_SENSORS];

uint8_t sensorIDs[MAX_SENSORS][8];
uint8_t sensorBusLoc[MAX_SENSORS]; 
float sensorTemps[MAX_SENSORS];
int foundSensors = 0;
bool sensorsConfigured = false;
unsigned long tempPreviousMillis = 0;
const long tempInterval = 2000;

/* =========================================
 * PART 3: I2C BINARY STRUCT
 * ========================================= */
struct SlavePayloadParent {
    float    soil_temp[5];
    uint16_t leaf_wetness;
    uint16_t rain_count;
    uint8_t  sensor_status;
    uint8_t  padding;
} __attribute__((packed));

SlavePayloadParent myPayload;
String dataToSend = "";

// --- PROTOTYPES ---
void setupSensors();
void discoverAllSensors(); 
void appendToFile(String data);
long calculateMedian(long* values, int count);
void requestEvent(); 
void IRAM_ATTR rainTipISR();

void setup() {
  Serial.begin(115200);
  delay(2000); 
  Serial.println("\n\n--- SLAVE SENSOR NODE STARTING ---");
  pinMode(PIN_POWER_LED, OUTPUT);
  pinMode(PIN_SENSOR_LED, OUTPUT);
  pinMode(PIN_DATA_LED, OUTPUT);
  analogWrite(PIN_POWER_LED, POWER_LED_BRIGHTNESS); 

  if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS Mount Failed");

  long startVal = touchRead(PIN_PROBE);
  for(int i=0; i<WINDOW_SIZE; i++) historyBuffer[i] = startVal;

  pinMode(LED_START_PIN, OUTPUT);
  pinMode(LED_READ_PIN, OUTPUT);
  
  // Initialize Rain Gauge Interrupt
  pinMode(PIN_RAIN_GAUGE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_RAIN_GAUGE), rainTipISR, FALLING);

  setupSensors();
  discoverAllSensors();

  Wire.begin((uint8_t)SLAVE_ADDR, I2C_SDA_PIN, I2C_SCL_PIN, 100000);             
  Wire.onRequest(requestEvent);         
  
  Serial.println("--- SLAVE READY AT ADDR 0x08 ---");
}

void loop() {
  unsigned long currentMillis = millis();

  // TASK 1: LEAF SENSOR
  if (currentMillis - leafPreviousMillis >= leafInterval) {
    leafPreviousMillis = currentMillis;
    long burstReadings[WINDOW_SIZE];
    for(int i=0; i<WINDOW_SIZE; i++){
      burstReadings[i] = touchRead(PIN_PROBE);
      delay(5);
    }
    historyBuffer[historyIndex] = calculateMedian(burstReadings, WINDOW_SIZE);
    historyIndex = (historyIndex + 1) % WINDOW_SIZE;
    latestLeafWetness = calculateMedian(historyBuffer, WINDOW_SIZE);
  }

  // TASK 2: TEMP SENSOR & DATA PREP
  if (currentMillis - tempPreviousMillis >= tempInterval) {
    tempPreviousMillis = currentMillis;
    digitalWrite(LED_READ_PIN, HIGH);
    
    if (sensorsConfigured) {
      for (int i = 0; i < foundSensors; i++) {
        sensors[sensorBusLoc[i]]->requestTemperatures();
      }
    }

    myPayload.leaf_wetness = latestLeafWetness;
    myPayload.rain_count = rainTips; // NOW MAPPED TO LIVE COUNTER
    myPayload.sensor_status = (foundSensors > 0) ? 1 : 0;
    myPayload.padding = 0;

    String currentData = String(latestLeafWetness);
    for (int i = 0; i < MAX_SENSORS; i++) {
      float t = ERROR_NO_DATA;
      if (i < foundSensors) {
        t = sensors[sensorBusLoc[i]]->getTempC(sensorIDs[i]);
        if (t == DEVICE_DISCONNECTED_C) t = ERROR_NO_DATA;
      }
      
      sensorTemps[i] = t;
      myPayload.soil_temp[i] = t; 
      
      currentData += ",";
      if (i < foundSensors) currentData += String(t, 1);
      else currentData += "0.0";
    }
    
    // Add rain count to the end of the CSV string
    currentData += "," + String(rainTips);
    dataToSend = currentData; 
    
    Serial.println("-------------------------------------------------");
    Serial.print("TIME: "); Serial.print(currentMillis / 1000); Serial.println("s");
    Serial.print("LEAF (Capacitive): "); Serial.println(latestLeafWetness);
    Serial.print("RAIN TIPS: "); Serial.println(rainTips);
    
    Serial.print("TEMPERATURES: [ ");
    for (int i = 0; i < MAX_SENSORS; i++) {
      if (i < foundSensors) Serial.print(sensorTemps[i], 1);
      else Serial.print("NC"); 
      if (i < MAX_SENSORS - 1) Serial.print(" | ");
    }
    Serial.println(" ]");
    Serial.println("STRUCT PACKED FOR I2C");
    Serial.println("-------------------------------------------------");
    
    digitalWrite(LED_READ_PIN, LOW);
    appendToFile(String(currentMillis/1000) + "," + dataToSend);
  }
}

// --- INTERRUPT SERVICE ROUTINE FOR RAIN GAUGE ---
void IRAM_ATTR rainTipISR() {
  unsigned long currentTime = millis();
  if ((currentTime - lastTipTime) > DEBOUNCE_TIME) {
    rainTips++;
    lastTipTime = currentTime;
  }
}

void requestEvent() {
  Wire.write((const uint8_t*)&myPayload, sizeof(SlavePayloadParent)); 
}

void setupSensors() {
  for (int i = 0; i < MAX_SENSORS; i++) {
    oneWire[i] = new OneWire(sensorPins[i]);
    sensors[i] = new DallasTemperature(oneWire[i]);
    sensors[i]->begin();
    sensors[i]->setWaitForConversion(false); 
    pinMode(sensorPins[i], INPUT_PULLUP);
  }
}

void discoverAllSensors() {
  digitalWrite(LED_START_PIN, HIGH);
  foundSensors = 0;
  for (int pinIndex = 0; pinIndex < MAX_SENSORS; pinIndex++) {
    DeviceAddress tempAddr;
    oneWire[pinIndex]->reset_search();
    if (oneWire[pinIndex]->search(tempAddr)) {
      if (tempAddr[0] == 0x28) {
        for (int j = 0; j < 8; j++) sensorIDs[foundSensors][j] = tempAddr[j];
        sensorBusLoc[foundSensors] = pinIndex;
        foundSensors++;
      }
    }
  }
  sensorsConfigured = (foundSensors > 0);
  digitalWrite(LED_START_PIN, LOW);
}

void appendToFile(String data) {
  File file = LittleFS.open(filename, "a"); 
  if (file) { file.println(data); file.close(); }
}

long calculateMedian(long* values, int count) {
  long temp[count];
  for(int i=0; i<count; i++) temp[i] = values[i];
  for(int i=0; i < count-1; i++) {
    for(int j=0; j < count-i-1; j++) {
      if(temp[j] > temp[j+1]) {
        long t = temp[j];
        temp[j] = temp[j+1]; temp[j+1] = t;
      }
    }
  }
  return temp[count/2];
}