#include <OneWire.h>
#include <DallasTemperature.h>

#define SENSOR_PIN 2
#define LED_READ_PIN 19
#define LED_START_PIN 23

OneWire oneWire(SENSOR_PIN);
DallasTemperature sensors(&oneWire);

// Your sensor IDs
uint8_t sensorIDs[5][8] = {
  {0x28, 0x6E, 0xAB, 0xA4, 0x00, 0x00, 0x00, 0x2A},
  {0x28, 0x0D, 0x57, 0x69, 0x00, 0x00, 0x00, 0x93},
  {0x28, 0xB3, 0x63, 0x68, 0x00, 0x00, 0x00, 0x93},
  {0x28, 0x4E, 0x46, 0x69, 0x00, 0x00, 0x00, 0x0E},
  {0x28, 0x0D, 0x53, 0x69, 0x00, 0x00, 0x00, 0x8C}
};

float sensorTemps[5];

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_START_PIN, OUTPUT);
  pinMode(LED_READ_PIN, OUTPUT);
  
  sensors.begin();
  
  // Blink LED_START_PIN at startup
  for(int i = 0; i < 5; i++) {
    digitalWrite(LED_START_PIN, HIGH);
    delay(200);
    digitalWrite(LED_START_PIN, LOW);
    delay(200);
  }
}

void loop() {
  // Turn on LED_READ_PIN during reading
  digitalWrite(LED_READ_PIN, HIGH);
  
  sensors.requestTemperatures();
  delay(750);
  
  // Read all sensors
  for(int i = 0; i < 5; i++) {
    float temp = sensors.getTempC(sensorIDs[i]);
    
    // Simple 0-100 range check
    if(temp >= 0.0 && temp <= 100.0) {
      sensorTemps[i] = temp;
    }else {
      sensorTemps[i] = 0.00; // Out of range
    }
  }
  
  digitalWrite(LED_READ_PIN, LOW);
  
  // Print results
  for(int i = 0; i < 5; i++) {
    if(sensorTemps[i] == 0.00) {
      Serial.print("ERR");
    } else {
      Serial.print(sensorTemps[i], 2);
    }
    
    if(i < 4) Serial.print(",");
  }
  
  Serial.println();
  
  delay(1000);
}