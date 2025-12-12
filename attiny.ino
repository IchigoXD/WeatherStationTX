// ATtiny85: reed switch counter as I2C slave with reset
#include <TinyWireS.h>

#define I2C_ADDR 0x12
#define CMD_RESET 0x01  // Command to reset counter

const uint8_t reedPin = 3; // PB3 (pin 2)
const uint8_t ledPin  = 1; // PB1 (pin 6)

volatile uint16_t counter = 0;
bool lastState = HIGH;
unsigned long lastDebounce = 0;
const unsigned long debounceDelay = 50;

unsigned long lastDetectTime = 0;
const unsigned long minReedInterval = 500;
bool magnetDetected = false;

void sendCounter() {
  TinyWireS.send((uint8_t)(counter & 0xFF));        // low byte
  TinyWireS.send((uint8_t)((counter >> 8) & 0xFF)); // high byte
}

// NEW: Function to handle received data
void receiveData(uint8_t numBytes) {
  if (numBytes > 0) {
    uint8_t command = TinyWireS.receive();  // Changed from read() to receive()
    
    if (command == CMD_RESET) {
      counter = 0;  // Reset the counter
      
      // Optional: Blink LED to confirm reset
      for (int i = 0; i < 3; i++) {
        digitalWrite(ledPin, HIGH);
        delay(100);
        digitalWrite(ledPin, LOW);
        delay(100);
      }
    }
  }
}

void setup() {
  pinMode(reedPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  TinyWireS.begin(I2C_ADDR);
  TinyWireS.onRequest(sendCounter);
  TinyWireS.onReceive(receiveData);  // NEW: Setup receive handler
}

void loop() {
  bool reading = digitalRead(reedPin);
  
  if (reading == LOW && !magnetDetected) {
    unsigned long now = millis();
    
    if ((now - lastDebounce) > debounceDelay && 
        (now - lastDetectTime) > minReedInterval) {
      magnetDetected = true;
      lastDebounce = now;
      lastDetectTime = now;
      
      counter++;
      
      digitalWrite(ledPin, HIGH);
      delay(80);
      digitalWrite(ledPin, LOW);
    }
  }
  
  if (reading == HIGH && magnetDetected) {
    if ((millis() - lastDebounce) > debounceDelay) {
      magnetDetected = false;
      lastDebounce = millis();
    }
  }
  
  lastState = reading;
  TinyWireS_stop_check();
}