#include <SPI.h>
#include <LoRa.h>
#include "DHT.h"

// Node eka Nama Dnna Onee Methennta
#define NODE_ID 1 

// DHT module Tikee Pins
#define DHTPIN 3
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

//Lora Module Eka Pins Tika
#define SS 10
#define RST 9
#define DIO0 2



void setup() {
  Serial.begin(9600);
  dht.begin();

  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed!");
    while (1);
  }
  Serial.println("Node Ready");
}



void loop() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) return;

  LoRa.beginPacket();
  LoRa.print("ID:");
  LoRa.print(NODE_ID);
  LoRa.print(" Temp:");
  LoRa.print(t);
  LoRa.print("C Hum:");
  LoRa.print(h);
  LoRa.print("%");
  LoRa.endPacket();


  Serial.println("Sent data from Node " + String(NODE_ID));

  //Update Cycles
  delay(5000);
}
