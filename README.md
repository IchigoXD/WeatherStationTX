# LoRa Weather Station

A simple weather data transmitter and receiver system using **LoRa SX1278 (433 MHz)** modules and Arduino done by students in university of Peradeniya.

---

## 📡 Features

- Transmit temperature and humidity data from multiple sensor nodes
- Central receiver collects data from all nodes
- Easy to expand with more nodes
- Optional: display data on Serial Monitor or OLED

---

## 🔧 Components

- Arduino Uno / Nano  
- SX1278 LoRa module (433 MHz)  
- DHT11 or DHT22 sensor (temperature & humidity)  
- Optional: BME280 (for pressure)  
- Jumper wires, breadboard, antenna

---

## ⚡ Wiring

**LoRa SX1278 → Arduino UNO**

| SX1278 Pin | Arduino Pin |
|------------|-------------|
| MISO       | D12         |
| MOSI       | D11         |
| SCK        | D13         |
| NSS (CS)   | D10         |
| DIO0       | D2          |
| VCC        | 3.3V        |
| GND        | GND         |

**DHT11 → Arduino**

| DHT Pin | Arduino Pin |
|---------|-------------|
| Data    | D3          |
| VCC     | 5V          |
| GND     | GND         |

---

## 💻 Usage

1. Upload **transmitter code** to each node.  
   - Change `NODE_ID` for each node.  
2. Upload **receiver code** to the hub Arduino.  
3. Open Serial Monitor on the hub to see incoming data.  
4. Optional: connect OLED to display live data.

---

## 🛠 Notes

- Always use **3.3V** for SX1278.  
- Use antennas to prevent module damage.  
- For multiple nodes, consider adding small random delays between transmissions to avoid collisions.

---

## 📖 License

MIT License  