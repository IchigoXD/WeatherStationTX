#include <SPI.h>
#include <LoRa.h>
#include "DHT.h"

#define DHTPIN 3
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

#define SS 10
#define RST 9
#define DIO0 2

void setup() {
  Serial.begin(9600);
  dht.begin();

  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("Starting LoRa failed!");
    while (1);
  }
  Serial.println("LoRa Sender Ready");
}

void loop() {
  
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("Sensor Error");
    return;
  }

  LoRa.beginPacket();
  LoRa.print("Temp:");
  LoRa.print(t);
  LoRa.print("C Hum:");
  LoRa.print(h);
  LoRa.print("%");
  LoRa.endPacket();

  Serial.println("Sent: " + String(t) + "C " + String(h) + "%");

  delay(2000);
}
