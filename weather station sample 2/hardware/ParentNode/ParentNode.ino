/*
 * ============================================================================
 *  Weather Station Network — Parent Node Firmware
 * ============================================================================
 *  Hardware: ATmega328P + AHT10 + ESP32 Slave (w/ rain gauge) + LoRa
 *            + W25Q64 SPI Flash + RTC + Siren via relay
 *
 *  Responsibilities:
 *    1. Read own sensors (AHT10 + ESP32 soil/leaf/rain)
 *    2. Receive data from 4 child nodes via LoRa
 *    3. Send ACK back to each child
 *    4. Relay all data sets (children + own) to Main station
 *    5. Activate siren if local OR any child security alert
 *    6. Respond to health checks from Main station
 *    7. Store unsent packets to flash with retry
 *
 *  Architecture: Fully non-blocking. NO delay() calls.
 *    - Security pin: hardware interrupt (INT0)
 *    - SINGLE LoRa.parsePacket() per loop — no buffer race condition
 *    - Siren: auto-disable after 5 minutes
 *    - AVR watchdog timer (8-second timeout)
 *    - Sensor reads split into millis()-based sub-states
 *    - Async LoRa TX via endPacket(true)
 *    - Sequence counter persisted to flash across reboots
 *    - Relay queue reduced to 3 entries to save SRAM
 * ============================================================================
 */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_AHT10.h>
#include "RTClib.h"
#include <avr/wdt.h>
#include "E:\weather station sample 2\hardware\Common\Config.h"
#include "E:\weather station sample 2\hardware\Common\FlashQueue.h"

#define MY_NODE_ID      NODE_PARENT
#define SIREN_TIMEOUT   300000UL    // Auto-off after 5 minutes

/* ── State Machine ─────────────────────────────────────────────────────── */
enum ParentState : uint8_t {
    PS_IDLE,
    PS_READING_AHT10,
    PS_WAIT_AHT10,
    PS_READING_ESP32,
    PS_WAIT_ESP32,
    PS_READING_DONE,
    PS_WAIT_SLOT,
    PS_SENDING,
    PS_WAIT_TX,
    PS_WAIT_ACK,
    PS_RETRY_BACKOFF,
    PS_QUEUE_FLUSH,
    PS_HEALTH_COLLECT
};

/* ── Globals ───────────────────────────────────────────────────────────── */
Adafruit_AHT10 aht;
RTC_DS3231 rtc;
FlashQueue flashQueue;

ParentState currentState      = PS_IDLE;
unsigned long stateEntryTime  = 0;
unsigned long lastReadTime    = 0;
unsigned long lastQueueCheck  = 0;
uint16_t sequenceCounter      = 0;

/* LED Flash Timers */
unsigned long ledLoraTimer = 0;
unsigned long ledGsmTimer  = 0;


/* Packet being transmitted to Main */
DataPacket txPacket;
uint8_t txRetryCount          = 0;
bool sendingFromQueue         = false;

/* Sensor read sub-state data */
uint8_t sensorStatus          = 0;

/* Relay queue: reduced to PARENT_RELAY_QUEUE_SIZE (3) to save SRAM */
DataPacket relayQueue[PARENT_RELAY_QUEUE_SIZE];
uint8_t relayQueueHead        = 0;
uint8_t relayQueueTail        = 0;
uint8_t relayQueueCount       = 0;

/* ── ACK flag — set by handleIncoming(), read by PS_WAIT_ACK ──────────── */
bool pendingAckFromMain       = false;

/* Security & Siren */
volatile bool securityTriggered = false;
bool localSecurityAlert        = false;   // PN-02: tracks own pin only
bool sirenActive              = false;
unsigned long sirenStartTime  = 0;

/* Sensor availability */
bool ahtAvailable  = false;
bool rtcAvailable  = false;
bool flashOK       = false;

/* Health check */
bool healthCheckActive        = false;
unsigned long healthStartTime = 0;
uint8_t healthAliveFlags      = 0;

/* ── Security Interrupt ────────────────────────────────────────────────── */
void securityISR() {
    securityTriggered = true;
}

/* ── Forward Declarations ──────────────────────────────────────────────── */
void doSendPacket(const DataPacket& p);
void handleIncoming();
void sendAckToChild(uint8_t childId, uint16_t seqId);
void enqueueRelay(const DataPacket& p);
bool dequeueRelay(DataPacket& p);
void handleHealthRequest(uint8_t requesterId);
void i2cBusRecovery();
void persistSequenceId();

/* ═══════════════════════════════════════════════════════════════════════════
 *  SETUP
 * ═══════════════════════════════════════════════════════════════════════════ */
void setup() {
    wdt_disable();

    Serial.begin(115200);
    Serial.println(F("[Parent] Initializing..."));

    /* Status LEDs */
    pinMode(PIN_LED_LORA, OUTPUT);
    pinMode(PIN_LED_GSM, OUTPUT);

    /* LED Boot Test — if they don't turn on here, the wiring is wrong! */
    digitalWrite(PIN_LED_LORA, HIGH);
    digitalWrite(PIN_LED_GSM, HIGH);
    { unsigned long t = millis(); while (millis() - t < 500) { wdt_reset(); } }
    digitalWrite(PIN_LED_LORA, LOW);
    digitalWrite(PIN_LED_GSM, LOW);

    /* Security pin — interrupt */
    pinMode(PIN_SECURITY, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_SECURITY), securityISR, FALLING);

    /* Siren relay — OFF initially */
    pinMode(PIN_SIREN_RELAY, OUTPUT);
    digitalWrite(PIN_SIREN_RELAY, LOW);

    /* I2C */
    Wire.begin();
    #if defined(WIRE_HAS_TIMEOUT)
    Wire.setWireTimeout(1000000UL, true);
    #endif

    /* AHT10 */
    ahtAvailable = aht.begin();
    Serial.print(F("  AHT10: "));
    Serial.println(ahtAvailable ? F("OK") : F("NOT FOUND"));

    /* RTC */
    rtcAvailable = rtc.begin();
    Serial.print(F("  RTC: "));
    Serial.println(rtcAvailable ? F("OK") : F("NOT FOUND"));

    /* Flash Queue (W25Q64) */
    SPI.begin();
    flashOK = flashQueue.begin(PIN_FLASH_CS, sizeof(DataPacket), PIN_LORA_CS);
    Serial.print(F("  SPI Flash: "));
    if (flashOK) {
        Serial.print(F("OK  ("));
        Serial.print(flashQueue.available());
        Serial.print(F("/"));
        Serial.print(flashQueue.maxEntries);
        Serial.println(F(" queued)"));

        /* Restore sequence counter from flash */
        sequenceCounter = flashQueue.loadSequenceId(SEQ_ID_FLASH_SECTOR);
        Serial.print(F("  SeqID restored: "));
        Serial.println(sequenceCounter);
    } else {
        Serial.println(F("NOT FOUND"));
    }

    /* LoRa */
    LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println(F("  LoRa: FAILED — halting"));
        while (1);
    }
    configureLora();
    Serial.println(F("  LoRa: OK"));

    currentState = PS_IDLE;
    lastReadTime = millis();
    Serial.println(F("[Parent] Ready.\n"));

    wdt_enable(WDTO_8S);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MAIN LOOP — SINGLE LoRa.parsePacket() via handleIncoming()
 * ═══════════════════════════════════════════════════════════════════════════ */
void loop() {
    wdt_reset();

    unsigned long now = millis();

    /* Non-blocking LED timeout (200ms flash) */
    if (ledLoraTimer > 0 && now - ledLoraTimer > 200) {
        digitalWrite(PIN_LED_LORA, LOW);
        ledLoraTimer = 0;
    }
    if (ledGsmTimer > 0 && now - ledGsmTimer > 200) {
        digitalWrite(PIN_LED_GSM, LOW);
        ledGsmTimer = 0;
    }

    /* ─── Security Monitoring (every iteration) ─── */
    if (securityTriggered) {
        securityTriggered = false;
        localSecurityAlert = true;  // PN-02: own pin only
        sirenActive = true;
        sirenStartTime = now;
        Serial.println(F("[SECURITY] Local pin — SIREN ON!"));
    }

    /* ─── Siren Management ─── */
    if (sirenActive) {
        digitalWrite(PIN_SIREN_RELAY, HIGH);
        if (now - sirenStartTime >= SIREN_TIMEOUT) {
            sirenActive = false;
            digitalWrite(PIN_SIREN_RELAY, LOW);
            Serial.println(F("[SIREN] Auto-off."));
        }
    } else {
        digitalWrite(PIN_SIREN_RELAY, LOW);
    }

    /* ─── Single LoRa parse — routes Data, ACK, Health ─── */
    handleIncoming();

    /* ─── Health check collection phase ─── */
    if (healthCheckActive) {
        if (now - healthStartTime >= HEALTH_CHECK_TIMEOUT) {
            healthAliveFlags |= (1 << (MY_NODE_ID - 1));  // Include ourselves

            HealthPacket resp;
            resp.magic      = MAGIC_BYTE;
            resp.type       = PKT_HEALTH_RESP;
            resp.sourceId   = MY_NODE_ID;
            resp.targetId   = NODE_MAIN;
            resp.aliveFlags = healthAliveFlags;
            resp.padding    = 0;
            healthPacketSetCRC(resp);

            digitalWrite(PIN_LED_LORA, HIGH);
            ledLoraTimer = millis();

            LoRa.beginPacket();
            LoRa.write((uint8_t*)&resp, sizeof(HealthPacket));
            LoRa.endPacket(true);   // Async TX

            Serial.print(F("[HEALTH] Sent flags=0x"));
            Serial.println(healthAliveFlags, HEX);
            healthCheckActive = false;
        }
    }

    /* ─── State Machine ─── */
    switch (currentState) {

        case PS_IDLE: {
            if (now - lastReadTime >= SENSOR_READ_INTERVAL) {
                /* Start sensor read sub-states */
                sensorStatus = 0;
                memset(&txPacket, 0, sizeof(DataPacket));
                txPacket.magic    = MAGIC_BYTE;
                txPacket.type     = PKT_DATA;
                txPacket.sourceId = MY_NODE_ID;
                txPacket.sequenceId = ++sequenceCounter;
                txPacket.rssi       = 0;
                txPacket.snr        = 0.0f;
                persistSequenceId();

                if (rtcAvailable) {
                    DateTime dt = rtc.now();
                    txPacket.timestamp = dt.unixtime();
                }

                currentState = PS_READING_AHT10;
                stateEntryTime = now;
            }
            else if (relayQueueCount > 0) {
                if (dequeueRelay(txPacket)) {
                    txRetryCount = 0;
                    sendingFromQueue = false;
                    currentState = PS_WAIT_SLOT;
                    stateEntryTime = now;
                }
            }
            else if (flashOK && now - lastQueueCheck >= QUEUE_CHECK_INTERVAL) {
                lastQueueCheck = now;
                if (!flashQueue.isEmpty()) {
                    currentState = PS_QUEUE_FLUSH;
                    stateEntryTime = now;
                }
            }
            break;
        }

        /* ── Sub-state: Read AHT10 ── */
        case PS_READING_AHT10: {
            if (ahtAvailable) {
                sensors_event_t humidity, temp;
                aht.getEvent(&humidity, &temp);

                bool tempOK = false;
                bool humOK  = false;

                if (!isnan(temp.temperature) &&
                    temp.temperature > -40.0f && temp.temperature < 80.0f) {
                    txPacket.data.temp = temp.temperature;
                    tempOK = true;
                } else {
                    txPacket.data.temp = 0.0f;
                }

                if (!isnan(humidity.relative_humidity) &&
                    humidity.relative_humidity >= 0.0f &&
                    humidity.relative_humidity <= 100.0f) {
                    txPacket.data.humidity = humidity.relative_humidity;
                    humOK = true;
                } else {
                    txPacket.data.humidity = 0.0f;
                }

                /* Set bit 0 only if BOTH temp and humidity are valid */
                if (tempOK && humOK) {
                    sensorStatus |= 0x01;
                }
            }
            currentState = PS_WAIT_AHT10;
            stateEntryTime = now;
            break;
        }

        case PS_WAIT_AHT10: {
            if (now - stateEntryTime >= SENSOR_READ_SUB_DELAY_MS) {
                currentState = PS_READING_ESP32;
                stateEntryTime = now;
            }
            break;
        }

        /* ── Sub-state: Read ESP32 Parent Slave ── */
        case PS_READING_ESP32: {
            SlavePayloadParent slaveData;
            memset(&slaveData, 0, sizeof(SlavePayloadParent));

            Wire.requestFrom((uint8_t)I2C_ESP32_SLAVE, (uint8_t)sizeof(SlavePayloadParent));
            if (Wire.available() == sizeof(SlavePayloadParent)) {
                uint8_t* ptr = (uint8_t*)&slaveData;
                for (size_t i = 0; i < sizeof(SlavePayloadParent); i++) {
                    ptr[i] = Wire.read();
                }
                memcpy(txPacket.data.soil_temp, slaveData.soil_temp, sizeof(slaveData.soil_temp));
                txPacket.data.leaf_wetness = slaveData.leaf_wetness;
                txPacket.data.rain_gauge   = slaveData.rain_count;
                sensorStatus |= slaveData.sensor_status;
            } else {
                while (Wire.available()) Wire.read();
                Serial.println(F("[I2C] ESP32 not responding"));
                i2cBusRecovery();
            }

            currentState = PS_WAIT_ESP32;
            stateEntryTime = now;
            break;
        }

        case PS_WAIT_ESP32: {
            if (now - stateEntryTime >= SENSOR_READ_SUB_DELAY_MS) {
                currentState = PS_READING_DONE;
            }
            break;
        }

        /* ── Sub-state: Finalize ── */
        case PS_READING_DONE: {
            /* PN-02: Report only LOCAL security pin state, not siren-from-child */
            txPacket.data.security_alert = localSecurityAlert ? 1 : 0;
            txPacket.data.sensor_status  = sensorStatus;
            dataPacketSetCRC(txPacket);

            Serial.print(F("[READ] T="));
            Serial.print(txPacket.data.temp, 1);
            Serial.print(F(" Rain="));
            Serial.println(txPacket.data.rain_gauge);

            enqueueRelay(txPacket);
            lastReadTime = now;
            localSecurityAlert = false;  // Clear after sending
            currentState = PS_IDLE;
            break;
        }

        case PS_WAIT_SLOT: {
            unsigned long cyclePos = now % TDMA_CYCLE_MS;
            if (tdmaIsMySlot(MY_NODE_ID, cyclePos)) {
                currentState = PS_SENDING;
                stateEntryTime = now;
            }
            break;
        }

        case PS_SENDING: {
            doSendPacket(txPacket);
            pendingAckFromMain = false;
            currentState = PS_WAIT_TX;
            stateEntryTime = now;
            break;
        }

        case PS_WAIT_TX: {
            /* 
             * LoRa.isTransmitting() is private in v0.8.0.
             * Using a fixed 150ms delay for async TX completion
             */
            if (now - stateEntryTime >= 150UL) {
                LoRa.receive();
                currentState = PS_WAIT_ACK;
                stateEntryTime = now;
            }
            break;
        }

        case PS_WAIT_ACK: {
            if (pendingAckFromMain) {
                pendingAckFromMain = false;
                Serial.println(F("[ACK] Main confirmed."));
                if (sendingFromQueue && flashOK) {
                    flashQueue.pop();
                }
                currentState = PS_IDLE;
            }
            else if (now - stateEntryTime >= ACK_TIMEOUT_MS) {
                txRetryCount++;
                digitalWrite(PIN_LED_GSM, HIGH);
                ledGsmTimer = now;

                Serial.print(F("[ACK] Main timeout. Retry "));
                Serial.print(txRetryCount);
                Serial.print(F("/"));
                Serial.println(MAX_RETRIES);

                if (txRetryCount >= MAX_RETRIES) {
                    if (flashOK && !sendingFromQueue) {
                        digitalWrite(PIN_LED_GSM, HIGH);
                        ledGsmTimer = now;
                        flashQueue.push((const uint8_t*)&txPacket);
                        Serial.println(F("[Flash] Saved for retry."));
                    }
                    currentState = PS_IDLE;
                } else {
                    /* Check if still within TDMA slot */
                    unsigned long cyclePos = now % TDMA_CYCLE_MS;
                    if (!tdmaIsMySlot(MY_NODE_ID, cyclePos)) {
                        if (flashOK && !sendingFromQueue) {
                            digitalWrite(PIN_LED_GSM, HIGH);
                            ledGsmTimer = now;
                            flashQueue.push((const uint8_t*)&txPacket);
                        }
                        currentState = PS_IDLE;
                    } else {
                        currentState = PS_RETRY_BACKOFF;
                        stateEntryTime = now;
                    }
                }
            }
            break;
        }

        case PS_RETRY_BACKOFF: {
            unsigned long backoff = RETRY_BACKOFF_BASE_MS * (1UL << (txRetryCount - 1));
            backoff += random(0, 500);
            if (now - stateEntryTime >= backoff) {
                currentState = PS_SENDING;
                stateEntryTime = now;
            }
            break;
        }

        case PS_QUEUE_FLUSH: {
            DataPacket queuedPkt;
            if (flashQueue.peek((uint8_t*)&queuedPkt)) {
                memcpy(&txPacket, &queuedPkt, sizeof(DataPacket));
                txRetryCount = 0;
                sendingFromQueue = true;
                currentState = PS_WAIT_SLOT;
                stateEntryTime = now;
            } else {
                currentState = PS_IDLE;
            }
            break;
        }

        case PS_HEALTH_COLLECT: {
            currentState = PS_IDLE;
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LoRa TRANSMIT (to Main Station) — async, non-blocking
 * ═══════════════════════════════════════════════════════════════════════════ */
void doSendPacket(const DataPacket& p) {
    digitalWrite(PIN_LED_LORA, HIGH);
    ledLoraTimer = millis();

    LoRa.beginPacket();
    LoRa.write((const uint8_t*)&p, sizeof(DataPacket));
    LoRa.endPacket(true);   // Async TX

    Serial.print(F("[TX→Main] src="));
    Serial.print(p.sourceId);
    Serial.print(F(" seq="));
    Serial.println(p.sequenceId);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  INCOMING PACKET HANDLER — SINGLE LoRa.parsePacket() per loop
 * ═══════════════════════════════════════════════════════════════════════════ */
void handleIncoming() {
    int packetSize = LoRa.parsePacket();
    if (packetSize == 0) return;

    /* ─── DataPacket from Child (52 bytes) ─── */
    if (packetSize == sizeof(DataPacket)) {
        DataPacket received;
        LoRa.readBytes((uint8_t*)&received, sizeof(DataPacket));

        if (received.magic != MAGIC_BYTE || !dataPacketVerifyCRC(received)) {
            Serial.println(F("[RX] Invalid DataPacket — dropped."));
            return;
        }

        if (received.type == PKT_DATA &&
            received.sourceId >= NODE_CHILD_1 &&
            received.sourceId <= NODE_CHILD_4) {

            Serial.print(F("[RX] Child "));
            Serial.print(received.sourceId);
            Serial.print(F(" seq="));
            Serial.println(received.sequenceId);

            /* Capture link quality for relay to Main/Server */
            received.rssi = LoRa.packetRssi();
            received.snr  = LoRa.packetSnr();
            dataPacketSetCRC(received); // Must re-sign because we modified fields

            digitalWrite(PIN_LED_LORA, HIGH);
            ledLoraTimer = millis();

            sendAckToChild(received.sourceId, received.sequenceId);

            if (received.data.security_alert) {
                sirenActive = true;
                sirenStartTime = millis();
                Serial.println(F("[SECURITY] Child alert — SIREN ON!"));
            }

            enqueueRelay(received);
        }
    }
    /* ─── ACK or HealthPacket (8 bytes) ─── */
    else if (packetSize == sizeof(AckPacket)) {
        uint8_t buf[sizeof(AckPacket)];
        LoRa.readBytes(buf, sizeof(AckPacket));

        uint8_t pktType = buf[1];

        if (pktType == PKT_ACK) {
            AckPacket ack;
            memcpy(&ack, buf, sizeof(AckPacket));

            if (ack.magic == MAGIC_BYTE &&
                ack.sourceId == NODE_MAIN &&
                ack.targetId == MY_NODE_ID &&
                ack.sequenceId == txPacket.sequenceId &&
                ackPacketVerifyCRC(ack)) {
                pendingAckFromMain = true;
                digitalWrite(PIN_LED_LORA, HIGH);
                ledLoraTimer = millis();
            }
        }
        else if (pktType == PKT_HEALTH_REQ) {
            HealthPacket hp;
            memcpy(&hp, buf, sizeof(HealthPacket));

            if (hp.magic == MAGIC_BYTE && healthPacketVerifyCRC(hp)) {
                handleHealthRequest(hp.sourceId);
            }
        }
        else if (pktType == PKT_HEALTH_RESP && healthCheckActive) {
            HealthPacket hp;
            memcpy(&hp, buf, sizeof(HealthPacket));

            if (hp.magic == MAGIC_BYTE && healthPacketVerifyCRC(hp)) {
                healthAliveFlags |= hp.aliveFlags;
                digitalWrite(PIN_LED_LORA, HIGH);
                ledLoraTimer = millis();
                Serial.print(F("[HEALTH] Got resp flags=0x"));
                Serial.println(healthAliveFlags, HEX);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEND ACK TO CHILD
 * ═══════════════════════════════════════════════════════════════════════════ */
void sendAckToChild(uint8_t childId, uint16_t seqId) {
    AckPacket ack;
    ack.magic      = MAGIC_BYTE;
    ack.type       = PKT_ACK;
    ack.sourceId   = MY_NODE_ID;
    ack.targetId   = childId;
    ack.sequenceId = seqId;
    ackPacketSetCRC(ack);

    digitalWrite(PIN_LED_LORA, HIGH);
    ledLoraTimer = millis();

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&ack, sizeof(AckPacket));
    LoRa.endPacket();       // Synchronous TX (blocks ~50ms)
    LoRa.receive();         // Immediately return to continuous RX

    Serial.print(F("[ACK→Child "));
    Serial.print(childId);
    Serial.print(F("] seq="));
    Serial.println(seqId);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RELAY QUEUE (in-RAM ring buffer, reduced size)
 * ═══════════════════════════════════════════════════════════════════════════ */
void enqueueRelay(const DataPacket& p) {
    if (relayQueueCount >= PARENT_RELAY_QUEUE_SIZE) {
        if (flashOK) {
            digitalWrite(PIN_LED_GSM, HIGH);
            ledGsmTimer = millis();
            flashQueue.push((const uint8_t*)&p);
            Serial.println(F("[QUEUE] Full — saved to Flash."));
        } else {
            Serial.println(F("[QUEUE] Full and no Flash — DROPPED!"));
        }
        return;
    }
    memcpy(&relayQueue[relayQueueTail], &p, sizeof(DataPacket));
    relayQueueTail = (relayQueueTail + 1) % PARENT_RELAY_QUEUE_SIZE;
    relayQueueCount++;
}

bool dequeueRelay(DataPacket& p) {
    if (relayQueueCount == 0) return false;
    memcpy(&p, &relayQueue[relayQueueHead], sizeof(DataPacket));
    relayQueueHead = (relayQueueHead + 1) % PARENT_RELAY_QUEUE_SIZE;
    relayQueueCount--;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HEALTH CHECK REQUEST HANDLER
 * ═══════════════════════════════════════════════════════════════════════════ */
void handleHealthRequest(uint8_t requesterId) {
    Serial.println(F("[HEALTH] Request — broadcasting..."));

    HealthPacket broadcast;
    broadcast.magic      = MAGIC_BYTE;
    broadcast.type       = PKT_HEALTH_REQ;
    broadcast.sourceId   = MY_NODE_ID;
    broadcast.targetId   = 0xFF;  // Broadcast
    broadcast.aliveFlags = 0;
    broadcast.padding    = 0;
    healthPacketSetCRC(broadcast);

    digitalWrite(PIN_LED_LORA, HIGH);
    ledLoraTimer = millis();

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&broadcast, sizeof(HealthPacket));
    LoRa.endPacket();       // Synchronous TX (blocks ~50ms)
    LoRa.receive();         // Immediately return to continuous RX

    healthCheckActive = true;
    healthStartTime   = millis();
    healthAliveFlags  = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PERSIST SEQUENCE COUNTER (every 10 increments)
 * ═══════════════════════════════════════════════════════════════════════════ */
void persistSequenceId() {
    if (flashOK && (sequenceCounter % 10 == 0)) {
        flashQueue.saveSequenceId(SEQ_ID_FLASH_SECTOR, sequenceCounter);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  I2C BUS RECOVERY
 * ═══════════════════════════════════════════════════════════════════════════ */
void i2cBusRecovery() {
    Serial.println(F("[I2C] Bus recovery..."));
    Wire.end();
    pinMode(A5, OUTPUT);
    for (uint8_t i = 0; i < 9; i++) {
        digitalWrite(A5, HIGH);
        delayMicroseconds(5);
        digitalWrite(A5, LOW);
        delayMicroseconds(5);
    }
    digitalWrite(A5, HIGH);
    pinMode(A5, INPUT_PULLUP);
    pinMode(A4, INPUT_PULLUP);
    Wire.begin();
    Serial.println(F("[I2C] Bus recovered."));
}
