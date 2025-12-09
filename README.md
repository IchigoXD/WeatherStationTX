### Dependencies
This project uses the following external libraries:

- SomeLibrary by @Author  
  Source: https://github.com/Author/SomeLibrary  
  License: MIT  

### Hardware Setup (Transmitter)
|SX1278 Pin|Arduino UNO Pin|
| -------- | ------- |
|MISO|D12|
|MOSI|D11|
|SCK|D13|
|NSS (CS)|D10|
|DIO0|D2|
|VCC|3.3V ⚠ (Very important)|
|GND|GND|

⚠ Do NOT power LoRa with 5V
⚠ Better to use a Level Shifter for SPI pins if possible
⚠ Use an antenna always → Without antenna module can burn 🔥

### Hardware Setup (Receiver)
🔧 Hardware Setup (Receiver)
|SX1278 Pin|Arduino UNO Pin|
| -------- | ------- |
|MISO|D12|
|MOSI|D11|
|SCK|D13|
|NSS (CS)|D10|
|DIO0|D2|
|VCC|3.3V ⚠|
|GND|GND|

⚠ Use 3.3V, and add an antenna.