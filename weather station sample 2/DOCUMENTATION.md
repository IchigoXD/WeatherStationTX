# Weather Station Network — Project Documentation

> **Version:** 2.0  
> **Date:** June 2026  
> **Platform:** ATmega328P + ESP32 + SIM800A + Flask/MySQL

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Materials and Methods](#2-materials-and-methods)
3. [Results](#3-results)
4. [Discussion](#4-discussion)
5. [Appendix](#5-appendix)

---

## 1. Introduction

### 1.1 Background

This project implements a multi-node wireless weather station network for environmental monitoring. The system collects temperature, humidity, soil temperature at multiple depths, leaf wetness, and rainfall data from distributed field stations. All data is transmitted wirelessly and stored in a central database with a web-based dashboard for real-time visualization.

### 1.2 Objectives

1. Build a network of **4 child stations**, **1 parent station**, and **1 main station**.
2. Collect sensor data every **2 minutes** from all nodes.
3. Transmit data using **LoRa radio** (433 MHz) with acknowledgement and retry mechanisms.
4. Upload data to a **Flask web server** via **SIM800A GSM/GPRS** module.
5. Store all data in a **MySQL database** and display it through a real-time web dashboard.
6. Implement **security monitoring** with siren activation on tampering.
7. Ensure **zero data loss** using flash memory queues for offline storage.

### 1.3 System Overview

```
┌─────────────┐   LoRa    ┌──────────────┐   LoRa    ┌──────────────┐   GPRS   ┌──────────┐
│ Child 1-4   │ ────────► │ Parent       │ ────────► │ Main         │ ───────► │ Server   │
│ (ATmega328P)│ ◄──ACK─── │ (ATmega328P) │ ◄──ACK─── │ (ATmega328P) │          │ (Flask)  │
│ + ESP32     │           │ + ESP32      │           │ + SIM800A    │          │ + MySQL  │
└─────────────┘           └──────────────┘           └──────────────┘          └──────────┘
```

---

## 2. Materials and Methods

### 2.1 Hardware Components

#### 2.1.1 Child Station (×4)

| Component | Purpose |
|-----------|---------|
| ATmega328P | Main controller |
| ESP32 | Sensor slave (I2C) — reads DS18B20 + leaf wetness |
| AHT10 | Air temperature and humidity |
| 5× DS18B20 | Soil temperature at depths: 0m, 20m, 40m, 60m, 80m |
| Capacitive leaf wetness sensor | Leaf moisture detection |
| DS3231 RTC | Real-time clock for timestamps |
| W25Q64 SPI Flash (8 MB) | Unsent packet storage (159,510 packet capacity) |
| SX1278 RA-02 LoRa (433 MHz) | Wireless communication |

#### 2.1.2 Parent Station (×1)

Same as child station, plus:

| Component | Purpose |
|-----------|---------|
| Tipping bucket rain gauge | Rainfall measurement (via ESP32 interrupt) |
| Relay module + siren | Security alarm activation |

#### 2.1.3 Main Station (×1)

| Component | Purpose |
|-----------|---------|
| ATmega328P | Main controller |
| SX1278 RA-02 LoRa (433 MHz) | Receives data from Parent |
| SIM800A GSM module (pins 0, 1) | GPRS data upload to server |
| W25Q64 SPI Flash (8 MB) | Unsent packet storage |
| DS3231 RTC | Timestamps |
| Status LEDs (A2, A3) | LoRa and Status/Error/GSM indicators |


#### 2.1.4 Pin Assignments (ATmega328P)

| Pin | Function |
|-----|----------|
| D2 (INT0) | Security tamper detection |
| D3 (INT1) | LoRa DIO0 interrupt |
| D4 | Siren relay (parent only) |
| D8 | W25Q64 flash chip select |
| D9 | LoRa reset |
| D10 | LoRa chip select (SPI SS) |
| A2 | LoRa activity LED indicator |
| A3 | Status/Error/GSM LED indicator |
| A4 | I2C SDA |
| A5 | I2C SCL |

### 2.2 Software Architecture

#### 2.2.1 Firmware Design Principles

All firmware follows strict non-blocking design:

- **No `delay()` calls** — millis()-based state machines throughout
- **Single `LoRa.parsePacket()` per loop** — prevents buffer race conditions
- **AVR Watchdog Timer** — 8-second timeout resets hung nodes
- **ESP32 Task Watchdog** — 10-second timeout for sensor slave
- **TDMA scheduling** — prevents LoRa transmission collisions

#### 2.2.2 State Machine (Child Node)

```
IDLE → READING_AHT10 → WAIT_AHT10 → READING_ESP32 → WAIT_ESP32
     → READING_DONE → WAIT_SLOT → SENDING → WAIT_TX → WAIT_ACK → IDLE
                                                     ↓ timeout
                                                   RETRY_BACKOFF → SENDING
                                                     ↓ max retries
                                                   FLASH_STORE → IDLE
```

#### 2.2.3 TDMA Schedule (60-Second Cycle)

| Time Slot | Node | Duration |
|-----------|------|----------|
| 0–12s | Child 1 | 12s (11.5s + 0.5s guard) |
| 12–24s | Child 2 | 12s |
| 24–36s | Child 3 | 12s |
| 36–48s | Child 4 | 12s |
| 48–60s | Parent | 12s |

#### 2.2.4 Data Packet Structure (52 bytes, packed)

| Field | Type | Size | Description |
|-------|------|------|-------------|
| magic | uint8_t | 1 | Network sync word (0xAF) |
| type | uint8_t | 1 | Packet type (DATA, ACK, HEALTH_REQ, HEALTH_RESP) |
| sourceId | uint8_t | 1 | Originating node ID (1–7) |
| reserved | uint8_t | 1 | Future use / alignment |
| sequenceId | uint16_t | 2 | Monotonically increasing per node |
| timestamp | uint32_t | 4 | Unix timestamp from RTC |
| temp | float | 4 | AHT10 temperature (°C) |
| humidity | float | 4 | AHT10 relative humidity (%) |
| soil_temp[5] | float[5] | 20 | DS18B20 at 0m, 20m, 40m, 60m, 80m |
| leaf_wetness | uint16_t | 2 | Capacitive sensor ADC value |
| rain_gauge | uint16_t | 2 | Tipping bucket count (parent only) |
| security_alert | uint8_t | 1 | 1 = security pin disconnected |
| sensor_status | uint8_t | 1 | Bitmask (bit0=AHT10, bit1=DS18B20, bit2=leaf, bit3=rain) |
| rssi | int16_t | 2 | LoRa signal strength (dBm) |
| snr | float | 4 | Signal-to-noise ratio (dB) |
| crc | uint16_t | 2 | CRC16-CCITT integrity check |

### 2.3 Communication Protocol

#### 2.3.1 LoRa Configuration

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Frequency | 433 MHz | ISM band |
| TX Power | 20 dBm | Maximum range (PA_BOOST) |
| Bandwidth | 125 kHz | Good balance of range and speed |
| Spreading Factor | SF9 | Good range for rural deployment |
| Coding Rate | 4/5 | Adequate FEC, lower air time |
| Sync Word | 0xAF | Network isolation from other LoRa devices |
| CRC | Enabled | Hardware-level error detection |

#### 2.3.2 Data Flow

1. **Child → Parent (LoRa):**
   - Child reads sensors every 2 minutes
   - Waits for its TDMA slot
   - Sends DataPacket with CRC16
   - Waits for ACK from Parent (3-second timeout)
   - On timeout: retries with exponential backoff (up to 3 attempts)
   - On max retries: saves to flash queue for later

2. **Parent → Main (LoRa):**
   - Receives child data, captures RSSI/SNR, sends ACK
   - Queues received packets + own readings in relay queue (3 entries RAM)
   - Overflow goes to flash queue
   - Relays to Main during its TDMA slot

3. **Main → Server (GPRS):**
   - Receives from Parent, sends ACK, stores to flash queue
   - Non-blocking AT command state machine uploads via HTTP POST
   - Raw binary DataPacket sent as `application/octet-stream`
   - Server responds with "OK" or "CHECK_ALIVE" command
   - Successful upload pops packet from flash queue

#### 2.3.3 Integrity Checks

- **CRC16-CCITT** over all packet bytes (except CRC field itself)
- **Magic byte** (0xAF) filters non-network transmissions
- **LoRa hardware CRC** enabled as additional layer
- **Node ID validation** prevents cross-node ACK confusion

#### 2.3.4 Health Check System

1. Server sends `CHECK_ALIVE` command in HTTP response body
2. Main Station receives and forwards LoRa health request to Parent
3. Parent broadcasts health request to all children
4. Children respond with their alive flags
5. Parent collects responses (10-second window), adds itself, replies to Main
6. Main uploads collected flags to server via `GET /api/health_response?flags=XX`
7. Server decodes bitmask: bit0=Child1, bit1=Child2, ..., bit4=Parent

### 2.4 Server Implementation

#### 2.4.1 Technology Stack

| Component | Technology |
|-----------|-----------|
| Web Framework | Python Flask |
| Database | MySQL 8.0 |
| Connection Pool | mysql-connector-python (32 connections) |
| Frontend | HTML/JavaScript with dynamic charts |
| Environment | dotenv for configuration |

#### 2.4.2 API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/upload` | Receive binary DataPacket from Main Station |
| GET | `/` | Dashboard UI |
| GET | `/api/readings` | Paginated readings with filters |
| GET | `/api/latest` | Latest reading per node |
| GET | `/api/averages` | Per-node averages (configurable hours) |
| GET | `/api/network_avg` | Network-wide combined averages |
| GET | `/api/time_series` | Time series data for charts |
| GET | `/api/node_status` | Node online/offline status |
| GET | `/api/alive_check` | Queue health check command |
| GET | `/api/health_response` | Receive health check results |
| GET | `/export` | Download readings as CSV file |

#### 2.4.3 Server-Side Features

- **Dew point calculation** using Magnus-Tetens formula
- **CRC16 verification** matching firmware implementation
- **15-minute deduplication window** for GPRS retry protection
- **SQL injection prevention** via field whitelisting
- **Optional API key** authentication for health endpoints
- **Streaming CSV export** for large datasets

### 2.5 Database Schema

#### 2.5.1 Readings Table

Stores all sensor data from all nodes. Indexed for efficient querying by node, time range, and sequence ID.

#### 2.5.2 Nodes Table

Registry of all 6 nodes (4 children + parent + main) with display names, types, and last-seen timestamps.

### 2.6 Memory Management

#### 2.6.1 ATmega328P SRAM Budget (2048 bytes)

| Component | Estimated Usage |
|-----------|----------------|
| DataPacket (current) | ~52 bytes |
| Stack + locals | ~250 bytes |
| Libraries (LoRa, Wire, AHT10, RTC) | ~1100 bytes |
| Relay queue (parent: 3 × 52 bytes) | ~156 bytes |
| **Total (worst case, parent)** | **~1558 bytes** |
| **Free** | **~490 bytes** |

#### 2.6.2 Flash Storage (W25Q64)

- **Total capacity:** 8 MB (2048 sectors × 4096 bytes)
- **Data area:** Sectors 1–2045 (2045 sectors)
- **Entries per sector:** 78 (for 52-byte packets)
- **Maximum queue depth:** 159,510 packets
- **Metadata sector:** Sector 0 (append-log, 256 entries before erase)
- **Sequence ID sector:** Sector 2046 (persistent counter)
- **Write endurance:** 100,000 erase cycles per sector

### 2.7 Sensor Reading Interval

- **Data collection interval:** 2 minutes (`SENSOR_READ_INTERVAL = 120000UL`)
- **ESP32 sensor sampling:** Every 2 seconds (cached for instant I2C response)
- **DS18B20 conversion time:** ~800ms (non-blocking async mode)

### 2.8 LED Status Indicators

All units (Child, Parent, and Main) feature two LEDs connected to pins **A2** (LoRa status) and **A3** (Status/Error/GSM) to provide real-time visual feedback of system events:

#### 2.8.1 Main Station
* **A2 (LoRa activity):** Flashes for 200ms when a packet is successfully received from the Parent Node (either Data or Health response).
* **A3 (GSM/GPRS activity):** Flashes for 200ms when transmitting data to the web server or when active GPRS communication is occurring.

#### 2.8.2 Parent Node
* **A2 (LoRa activity):** Flashes for 200ms when transmitting (sending DataPacket to Main, forwarding Ack to Child, or broadcasting Health check request) or receiving (receiving Child DataPacket, receiving Main's Ack, or Child Health check responses).
* **A3 (Status/Error):** Flashes for 200ms when a transmission failure occurs (ACK timeout from Main), and turns solid when saving/appending data to local SPI Flash queue or when the relay queue overflows.

#### 2.8.3 Child Nodes (1–4)
* **A2 (LoRa activity):** Flashes for 200ms when transmitting (sending DataPacket or responding to Health check) or receiving (receiving ACK from the Parent Node).
* **A3 (Status/Error):** Flashes for 200ms when a transmission failure occurs (ACK timeout from Parent), and turns solid when saving data to local SPI Flash queue or when out-of-slot timing forces a queue push.

---

## 3. Results

### 3.1 Code Audit Findings

A comprehensive audit was performed on all firmware and server code. **4 bugs/remediations were completed:**

| Severity | Issue | File | Fix |
|----------|-------|------|-----|
| 🔴 Critical | LoRa init failure did not halt the MCU | MainStation.ino | Added `while(1) { wdt_reset(); }` |
| 🔴 Critical | Database UNIQUE KEY will block inserts after sequence wrap | database_manager.py | Replaced with composite INDEX |
| 🟡 Moderate | `delay(500)` used in Main Station setup | MainStation.ino | Replaced with millis()-based wait |
| 🟢 Minor | Child and Parent nodes lacked LED status indications | ChildNode.ino, ParentNode.ino | Implemented A2 (LoRa) and A3 (Status) LED signaling |

Additionally, **6 documentation errors** were corrected (DataPacket size references updated from 46 to 52 bytes throughout).

### 3.2 Verified Features

All critical subsystems passed review:

- Non-blocking state machines (Child, Parent, Main)
- TDMA collision avoidance
- CRC16 integrity verification (firmware + server)
- Flash queue reliability (push/peek/pop with metadata persistence)
- Security pin interrupt monitoring
- Health check end-to-end flow
- Server-side deduplication and data parsing

---

## 4. Discussion

### 4.1 Strengths

1. **Robust non-blocking design** — No `delay()` calls anywhere in the codebase. Security pin monitoring is continuous, and the watchdog timer prevents firmware hangs.

2. **Multi-layer data integrity** — Magic byte filtering, CRC16-CCITT, LoRa hardware CRC, and server-side CRC verification provide strong protection against data corruption.

3. **Zero data loss architecture** — Flash queue (159,510 packet capacity) stores unsent packets for retry. Even if GPRS is down for weeks, no data is lost.

4. **TDMA scheduling** — Eliminates LoRa transmission collisions between nodes.

5. **Sequence counter persistence** — Surviving reboots prevents duplicate detection failures at the server.

### 4.2 Known Limitations

1. **Sequence counter wraps at 65535** (~455 days at 2-minute intervals). The dedup window is 15 minutes, so this is handled correctly, but the counter could overlap with historical data.

2. **SIM800A on hardware serial (pins 0, 1)** means debug output requires a conditional SoftwareSerial, which can interfere with LoRa interrupts. Debug mode is disabled by default.

3. **Single Parent node** is a single point of failure. If the Parent goes offline, all child data is queued locally until it recovers.

4. **AT command timeouts** are fixed values. In poor GSM coverage, the GPRS state machine may cycle through reconnections.

### 4.3 Recommendations for Future Work

1. Add over-the-air (OTA) firmware update capability for ESP32 slaves
2. Implement sleep modes for battery-powered deployments
3. Add GPS module for automatic time synchronization (alternative to RTC)
4. Consider adding a second Parent node for redundancy
5. Add data compression (delta encoding) to reduce LoRa airtime

---

## 5. Appendix

### 5.1 File Structure

```
weather station sample 2/
├── hardware/
│   ├── Common/
│   │   ├── Config.h           # Network configuration (node ID, pins, timing)
│   │   ├── Protocol.h         # Packet structures, CRC16, TDMA
│   │   ├── FlashQueue.h       # W25Q64 SPI flash circular queue
│   │   └── EEPROMQueue.h      # AT24C256 I2C EEPROM queue (alternative)
│   ├── ChildNode/
│   │   └── ChildNode.ino      # Child station firmware
│   ├── ParentNode/
│   │   └── ParentNode.ino     # Parent station firmware
│   ├── MainStation/
│   │   └── MainStation.ino    # Main station firmware (SIM800A + LoRa)
│   └── ESP32Slave/
│       ├── ESP32Slave.ino       # ESP32 sensor slave (child variant)
│       └── ESP32SlaveParent.ino # ESP32 sensor slave (parent variant)
├── server/
│   ├── app.py                 # Flask web server
│   ├── database_manager.py    # MySQL database interface
│   ├── schema.sql             # Database schema
│   ├── requirements.txt       # Python dependencies
│   ├── .env                   # Environment configuration
│   ├── templates/             # HTML templates
│   └── static/                # CSS, JS, assets
└── target.txt                 # Project requirements
```

### 5.2 Configuration Guide

Before flashing each node, set the node ID in `Config.h`:

```c
#define THIS_NODE_ID    NODE_CHILD_1    // Change for each physical station
```

Or use the compiler flag: `-DTHIS_NODE_ID=NODE_CHILD_2`

**Available IDs:** `NODE_CHILD_1` (1), `NODE_CHILD_2` (2), `NODE_CHILD_3` (3), `NODE_CHILD_4` (4), `NODE_PARENT` (5), `NODE_MAIN` (6)

### 5.3 Server Setup

```bash
# Install dependencies
pip install -r requirements.txt

# Configure database (edit .env)
DB_HOST=localhost
DB_USER=root
DB_PASSWORD=your_password
DB_NAME=weather_station

# Run server
python app.py
```

### 5.4 GSM Configuration

- **APN:** `airtelgprs.com`
- **Server URL:** Set `SERVER_URL` in Config.h to your server's public address
- **Module:** SIM800A connected to ATmega328P pins 0 (RX) and 1 (TX)
- **Baud rate:** 9600

### 5.5 Sensor Status Bitmask

| Bit | Sensor | Description |
|-----|--------|-------------|
| 0 | AHT10 | Air temp + humidity OK |
| 1 | DS18B20 | At least one soil probe OK |
| 2 | Leaf wetness | Sensor reading > 0 |
| 3 | Rain gauge | Tip count > 0 (parent only) |
