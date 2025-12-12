#include <Wire.h>

const uint8_t ATTN_ADDR = 0x12;    // ATtiny address
const uint8_t RESET_CMD = 0x01;    // Reset command

uint16_t lastCount = 0;
bool firstRead = true;  // Flag for first read

void setup() {
  Serial.begin(9600);
  Wire.begin();
  
  Serial.println("ATtiny85 Counter - Ready");
  Serial.println("Commands: r=reset, s=manual status check");
}

// Read counter from ATtiny
uint16_t readCounter() {
  uint16_t count = 0xFFFF;
  
  Wire.requestFrom(ATTN_ADDR, 2);
  if (Wire.available() >= 2) {
    uint8_t low = Wire.read();
    uint8_t high = Wire.read();
    count = (high << 8) | low;
  }
  
  return count;
}

// Reset counter on ATtiny
void resetCounter() {
  Wire.beginTransmission(ATTN_ADDR);
  Wire.write(RESET_CMD);
  byte error = Wire.endTransmission();
  
  if (error == 0) {
    Serial.println("Counter reset to 0");
    lastCount = 0;  // Reset local count too
  } else {
    Serial.println("Reset failed - check connection");
  }
}

// Manually check status
void checkStatus() {
  uint16_t count = readCounter();
  
  if (count == 0xFFFF) {
    Serial.println("Error: Cannot read from ATtiny");
  } else {
    Serial.print("Current count: ");
    Serial.println(count);
  }
}

void loop() {
  // Read counter from ATtiny
  uint16_t currentCount = readCounter();
  
  if (currentCount != 0xFFFF) {  // Valid reading
    if (firstRead) {
      // Show initial count on startup
      Serial.print("Initial count: ");
      Serial.println(currentCount);
      lastCount = currentCount;
      firstRead = false;
    } 
    else if (currentCount != lastCount) {
      // Only print when count changes
      Serial.print("New count: ");
      Serial.println(currentCount);
      lastCount = currentCount;
    }
  }
  
  // Handle serial commands
  if (Serial.available()) {
    char cmd = Serial.read();
    
    if (cmd == 'r' || cmd == 'R') {
      resetCounter();
    } 
    else if (cmd == 's' || cmd == 'S') {
      checkStatus();
    }
    else if (cmd != '\n' && cmd != '\r') {
      Serial.println("Commands: r=reset, s=status");
    }
  }
  
  delay(200);  // Small delay between reads
}