#include <SPI.h>
#include <LoRa.h>

//Lora Module Eka Pins Tika
#define SS 10
#define RST 9
#define DIO0 2

void setup() {
  Serial.begin(9600);
  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed!");
    while (1);
  }
  Serial.println("Hub Ready");
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String data = "";
    while (LoRa.available()) {
      data += (char)LoRa.read();
    }
    Serial.println("Received: " + data);
  }
}
