#include <SPI.h>
#include <LoRa.h>

#define SS 10
#define RST 9
#define DIO0 2

void setup() {
  Serial.begin(9600);
  while (!Serial);

  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("Starting LoRa failed!");
    while (1);
  }

  Serial.println("LoRa Receiver Ready");
}

void loop() {
  // Try to parse a LoRa packet
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String received = "";

    while (LoRa.available()) {
      received += (char)LoRa.read();
    }

    Serial.print("Received: ");
    Serial.println(received);
  }
}
