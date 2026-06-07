/*
 * ============================================================================
 *  Weather Station Network — Child Node Firmware
 * ============================================================================
 *  Hardware: ATmega328P + AHT10 + ESP32 Slave (I2C) + LoRa + W25Q64 + RTC
 *
 *  Architecture: Fully non-blocking state machine.
 *    - NO delay() calls anywhere (security pin monitored continuously)
 *    - Security pin uses hardware interrupt (INT0)
 *    - TDMA-based LoRa transmission to avoid collisions
 *    - W25Q64 SPI Flash for unsent packets with automatic retry
 *    - Exponential backoff on ACK timeout
 *    - CRC16 integrity on all packets
 *    - SINGLE LoRa.parsePacket() per loop — no buffer race condition
 *    - AVR watchdog timer (8-second timeout)
 *    - Sensor reads split into millis()-based sub-states
 *    - Async LoRa TX via endPacket(true)
 *    - Sequence counter persisted to flash across reboots
 *
 *  State Machine:
 *    IDLE → READING_AHT10 → WAIT_AHT10 → READING_ESP32 → WAIT_ESP32
 *         → READING_DONE → WAIT_SLOT → SENDING → WAIT_TX → WAIT_ACK → IDLE
 *                                                          → RETRY (backoff)
 *                                                          → FLASH_STORE
 *
 *  Memory Budget (ATmega328P = 2048 bytes SRAM):
 *    - DataPacket:     ~52 bytes
 *    - Stack/locals:   ~250 bytes
 *    - Libraries:      ~1100 bytes (LoRa, Wire, AHT10, RTC)
 *    - Total:          ~1396 bytes (safe margin — no SD.h = +512 bytes free)
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

/* ── Compile-time node ID (change in Config.h or via -DTHIS_NODE_ID=...) ── */
#if THIS_NODE_ID < NODE_CHILD_1 || THIS_NODE_ID > NODE_CHILD_4
  #error "THIS_NODE_ID must be NODE_CHILD_1..NODE_CHILD_4 for ChildNode firmware"
#endif

/* ── State Machine ─────────────────────────────────────────────────────── */
enum NodeState : uint8_t {
    STATE_IDLE,
    STATE_READING_AHT10,
    STATE_WAIT_AHT10,
    STATE_READING_ESP32,
    STATE_WAIT_ESP32,
    STATE_READING_DONE,
    STATE_WAIT_SLOT,
    STATE_SENDING,
    STATE_WAIT_TX,
    STATE_WAIT_ACK,
    STATE_RETRY_BACKOFF,
    STATE_QUEUE_FLUSH,
    STATE_HEALTH_RESPOND
};

/* ── Globals ───────────────────────────────────────────────────────────── */
Adafruit_AHT10 aht;
RTC_DS3231 rtc;
FlashQueue flashQueue;

NodeState currentState        = STATE_IDLE;
DataPacket currentPacket;
uint16_t sequenceCounter      = 0;
unsigned long lastReadTime    = 0;
unsigned long stateEntryTime  = 0;
uint8_t retryCount            = 0;

/* Security */
volatile bool securityTriggered = false;
bool securityAlertPending       = false;

/* ── ACK flag — set by handleIncoming(), read by STATE_WAIT_ACK ───────── */
bool pendingAckReceived         = false;
bool sendingFromQueue           = false;

/* Queue check timing */
unsigned long lastQueueCheck  = 0;

/* LED Flash Timers */
unsigned long ledLoraTimer = 0;
unsigned long ledGsmTimer  = 0;


/* Sensor availability */
bool ahtAvailable  = false;
bool rtcAvailable  = false;
bool flashOK       = false;

/* Sensor read sub-state data */
uint8_t sensorStatus = 0;

/* ── Security Interrupt (INT0 on pin 2) ────────────────────────────────── */
void securityISR() {
    securityTriggered = true;
}

/* ── Forward Declarations ──────────────────────────────────────────────── */
void doSendPacket();
void handleIncoming();
void handleHealthCheck(uint8_t requesterId);
void i2cBusRecovery();
void persistSequenceId();

/* ═══════════════════════════════════════════════════════════════════════════
 *  SETUP
 * ═══════════════════════════════════════════════════════════════════════════ */
void setup() {
    /* Disable watchdog during setup (in case of WDT-caused reboot) */
    wdt_disable();

    Serial.begin(115200);
    Serial.print(F("[Child "));
    Serial.print(THIS_NODE_ID);
    Serial.println(F("] Initializing..."));

    /* Status LEDs */
    pinMode(PIN_LED_LORA, OUTPUT);
    pinMode(PIN_LED_GSM, OUTPUT);

    /* LED Boot Test — if they don't turn on here, the wiring is wrong! */
    digitalWrite(PIN_LED_LORA, HIGH);
    digitalWrite(PIN_LED_GSM, HIGH);
    { unsigned long t = millis(); while (millis() - t < 500) { wdt_reset(); } }
    digitalWrite(PIN_LED_LORA, LOW);
    digitalWrite(PIN_LED_GSM, LOW);

    /* Security pin — hardware interrupt, continuous monitoring */
    pinMode(PIN_SECURITY, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_SECURITY), securityISR, FALLING);

    /* I2C bus for AHT10 + ESP32 slave */
    Wire.begin();
    #if defined(WIRE_HAS_TIMEOUT)
    Wire.setWireTimeout(1000000UL, true);  // 1 second timeout, reset on timeout
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

    currentState = STATE_IDLE;
    lastReadTime = millis();

    Serial.println(F("[Child] Ready.\n"));

    /* Enable watchdog (8-second timeout) */
    wdt_enable(WDTO_8S);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MAIN LOOP — Non-blocking state machine
 *
 *  CRITICAL: handleIncoming() is the ONLY place that calls
 *  LoRa.parsePacket(). This eliminates the buffer race condition
 *  where a separate doCheckAck() would see an empty buffer.
 * ═══════════════════════════════════════════════════════════════════════════ */
void loop() {
    wdt_reset();    // Feed the watchdog every loop iteration

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

    /* ─── Security Monitoring (always, every iteration) ─── */
    if (securityTriggered) {
        securityTriggered = false;
        securityAlertPending = true;
        Serial.println(F("[SECURITY] Pin disconnected!"));
        if (currentState == STATE_IDLE) {
            currentState = STATE_READING_AHT10;
            stateEntryTime = now;
        }
    }

    /* ─── Single LoRa parse per loop — routes ACK, Health, etc. ─── */
    handleIncoming();

    /* ─── State Machine ─── */
    switch (currentState) {

        case STATE_IDLE: {
            if (now - lastReadTime >= SENSOR_READ_INTERVAL) {
                currentState = STATE_READING_AHT10;
                stateEntryTime = now;
                sensorStatus = 0;
                memset(&currentPacket, 0, sizeof(DataPacket));
                currentPacket.magic    = MAGIC_BYTE;
                currentPacket.type     = PKT_DATA;
                currentPacket.sourceId = THIS_NODE_ID;
                currentPacket.sequenceId = ++sequenceCounter;
                currentPacket.rssi       = 0;
                currentPacket.snr        = 0.0f;

                /* Persist sequence counter to flash */
                persistSequenceId();

                /* RTC Timestamp */
                if (rtcAvailable) {
                    DateTime dt = rtc.now();
                    currentPacket.timestamp = dt.unixtime();
                }
            }
            else if (flashOK && now - lastQueueCheck >= QUEUE_CHECK_INTERVAL) {
                lastQueueCheck = now;
                if (!flashQueue.isEmpty()) {
                    currentState = STATE_QUEUE_FLUSH;
                    stateEntryTime = now;
                }
            }
            break;
        }

        /* ── Sub-state: Read AHT10 (I2C) ── */
        case STATE_READING_AHT10: {
            if (ahtAvailable) {
                sensors_event_t humidity, temp;
                aht.getEvent(&humidity, &temp);

                /* Validate readings — reject NaN and out-of-range */
                bool tempOK = false;
                bool humOK  = false;

                if (!isnan(temp.temperature) &&
                    temp.temperature > -40.0f && temp.temperature < 80.0f) {
                    currentPacket.data.temp = temp.temperature;
                    tempOK = true;
                } else {
                    currentPacket.data.temp = 0.0f;
                }

                if (!isnan(humidity.relative_humidity) &&
                    humidity.relative_humidity >= 0.0f &&
                    humidity.relative_humidity <= 100.0f) {
                    currentPacket.data.humidity = humidity.relative_humidity;
                    humOK = true;
                } else {
                    currentPacket.data.humidity = 0.0f;
                }

                /* CN-01: Set bit 0 only if BOTH temp and humidity are valid */
                if (tempOK && humOK) {
                    sensorStatus |= 0x01;
                }
            }

            currentState = STATE_WAIT_AHT10;
            stateEntryTime = now;
            break;
        }

        /* ── Sub-state: Wait between AHT10 and ESP32 reads ── */
        case STATE_WAIT_AHT10: {
            if (now - stateEntryTime >= SENSOR_READ_SUB_DELAY_MS) {
                currentState = STATE_READING_ESP32;
                stateEntryTime = now;
            }
            break;
        }

        /* ── Sub-state: Read ESP32 Slave (I2C) ── */
        case STATE_READING_ESP32: {
            SlavePayloadChild slaveData;
            memset(&slaveData, 0, sizeof(SlavePayloadChild));

            Wire.requestFrom((uint8_t)I2C_ESP32_SLAVE, (uint8_t)sizeof(SlavePayloadChild));
            if (Wire.available() == sizeof(SlavePayloadChild)) {
                uint8_t* ptr = (uint8_t*)&slaveData;
                for (size_t i = 0; i < sizeof(SlavePayloadChild); i++) {
                    ptr[i] = Wire.read();
                }
                memcpy(currentPacket.data.soil_temp, slaveData.soil_temp, sizeof(slaveData.soil_temp));
                currentPacket.data.leaf_wetness = slaveData.leaf_wetness;
                sensorStatus |= slaveData.sensor_status;
            } else {
                while (Wire.available()) Wire.read();
                Serial.println(F("[I2C] ESP32 not responding"));
                i2cBusRecovery();
            }

            currentState = STATE_WAIT_ESP32;
            stateEntryTime = now;
            break;
        }

        /* ── Sub-state: Wait after ESP32 read ── */
        case STATE_WAIT_ESP32: {
            if (now - stateEntryTime >= SENSOR_READ_SUB_DELAY_MS) {
                currentState = STATE_READING_DONE;
            }
            break;
        }

        /* ── Sub-state: Finalize packet ── */
        case STATE_READING_DONE: {
            /* Rain gauge — not present on child nodes */
            currentPacket.data.rain_gauge = 0;

            /* Security Alert */
            currentPacket.data.security_alert = securityAlertPending ? 1 : 0;

            /* Finalize */
            currentPacket.data.sensor_status = sensorStatus;
            dataPacketSetCRC(currentPacket);

            Serial.print(F("[READ] T="));
            Serial.print(currentPacket.data.temp, 1);
            Serial.print(F(" H="));
            Serial.print(currentPacket.data.humidity, 1);
            Serial.print(F(" Seq="));
            Serial.println(currentPacket.sequenceId);

            retryCount = 0;
            sendingFromQueue = false;
            currentState = STATE_WAIT_SLOT;
            stateEntryTime = now;
            break;
        }

        case STATE_WAIT_SLOT: {
            unsigned long cyclePos = now % TDMA_CYCLE_MS;
            if (tdmaIsMySlot(THIS_NODE_ID, cyclePos)) {
                currentState = STATE_SENDING;
                stateEntryTime = now;
            }
            break;
        }

        case STATE_SENDING: {
            doSendPacket();
            pendingAckReceived = false;  // Clear before waiting
            currentState = STATE_WAIT_TX;
            stateEntryTime = now;
            break;
        }

        /* ── Wait for async LoRa TX to complete ── */
        case STATE_WAIT_TX: {
            /* 
             * LoRa.isTransmitting() is private in v0.8.0.
             * Using a fixed 150ms delay for async TX completion
             * (SF7 52-byte pkt takes ~100ms).
             */
            if (now - stateEntryTime >= 150UL) {
                LoRa.receive();
                currentState = STATE_WAIT_ACK;
                stateEntryTime = now;
            }
            break;
        }

        case STATE_WAIT_ACK: {
            /*
             * ACK is detected by handleIncoming() (called above)
             * which sets pendingAckReceived = true.
             * No second LoRa.parsePacket() call here!
             */
            if (pendingAckReceived) {
                pendingAckReceived = false;
                Serial.println(F("[ACK] Confirmed."));
                /* Only clear security alert if this packet carried one */
                if (currentPacket.data.security_alert) {
                    securityAlertPending = false;
                }
                lastReadTime = now;
                currentState = STATE_IDLE;
            }
            else if (now - stateEntryTime >= ACK_TIMEOUT_MS) {
                retryCount++;
                digitalWrite(PIN_LED_GSM, HIGH);
                ledGsmTimer = now;

                Serial.print(F("[ACK] Timeout. Retry "));
                Serial.print(retryCount);
                Serial.print(F("/"));
                Serial.println(MAX_RETRIES);

                if (retryCount >= MAX_RETRIES) {
                    Serial.println(F("[ACK] Max retries — saving to Flash."));
                    if (flashOK && !sendingFromQueue) {
                        flashQueue.push((const uint8_t*)&currentPacket);
                    }
                    lastReadTime = now;
                    currentState = STATE_IDLE;
                } else {
                    /* Check if we're still within our TDMA slot before retrying */
                    unsigned long cyclePos = now % TDMA_CYCLE_MS;
                    if (!tdmaIsMySlot(THIS_NODE_ID, cyclePos)) {
                        /* Out of slot — save to flash, retry next cycle */
                        if (flashOK && !sendingFromQueue) {
                            flashQueue.push((const uint8_t*)&currentPacket);
                        }
                        lastReadTime = now;  // CN-02: prevent immediate re-read
                        currentState = STATE_IDLE;
                    } else {
                        currentState = STATE_RETRY_BACKOFF;
                        stateEntryTime = now;
                    }
                }
            }
            break;
        }

        case STATE_RETRY_BACKOFF: {
            unsigned long backoff = RETRY_BACKOFF_BASE_MS * (1UL << (retryCount - 1));
            backoff += random(0, 500);
            if (now - stateEntryTime >= backoff) {
                currentState = STATE_SENDING;
                stateEntryTime = now;
            }
            break;
        }

        case STATE_QUEUE_FLUSH: {
            DataPacket queuedPacket;
            if (flashQueue.peek((uint8_t*)&queuedPacket)) {
                memcpy(&currentPacket, &queuedPacket, sizeof(DataPacket));
                retryCount = 0;
                sendingFromQueue = true;
                currentState = STATE_WAIT_SLOT;
                stateEntryTime = now;
            } else {
                currentState = STATE_IDLE;
            }
            break;
        }

        case STATE_HEALTH_RESPOND: {
            currentState = STATE_IDLE;
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LoRa TRANSMIT (async — non-blocking)
 * ═══════════════════════════════════════════════════════════════════════════ */
void doSendPacket() {
    digitalWrite(PIN_LED_LORA, HIGH);
    ledLoraTimer = millis();

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&currentPacket, sizeof(DataPacket));
    LoRa.endPacket(true);   // true = async, non-blocking TX

    Serial.print(F("[TX] Sent seq="));
    Serial.print(currentPacket.sequenceId);
    Serial.print(F(" src="));
    Serial.println(currentPacket.sourceId);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  INCOMING PACKET HANDLER — SINGLE LoRa.parsePacket() per loop
 *
 *  This is the ONLY function that reads from the LoRa buffer.
 *  It routes packets to the correct handler by type:
 *    - PKT_ACK        → sets pendingAckReceived flag
 *    - PKT_HEALTH_REQ → sends health response
 *
 *  Because AckPacket and HealthPacket are the same size (8 bytes),
 *  we read the raw bytes first, then check the type field.
 * ═══════════════════════════════════════════════════════════════════════════ */
void handleIncoming() {
    int packetSize = LoRa.parsePacket();
    if (packetSize == 0) return;

    /*
     * DataPacket = 52 bytes, AckPacket = HealthPacket = 8 bytes.
     * Differentiate by size first, then by type field.
     */
    if (packetSize == sizeof(AckPacket)) {
        /* Could be ACK or HealthPacket — read raw, check type */
        uint8_t buf[sizeof(AckPacket)];
        LoRa.readBytes(buf, sizeof(AckPacket));

        uint8_t pktType = buf[1];  /* type field is at byte offset 1 */

        if (pktType == PKT_ACK) {
            AckPacket ack;
            memcpy(&ack, buf, sizeof(AckPacket));

            if (ack.magic == MAGIC_BYTE &&
                ack.targetId == THIS_NODE_ID &&
                ack.sequenceId == currentPacket.sequenceId &&
                ackPacketVerifyCRC(ack)) {
                pendingAckReceived = true;
                digitalWrite(PIN_LED_LORA, HIGH);
                ledLoraTimer = millis();

                /* Pop from Flash queue only if this was a queued transmission */
                if (currentState == STATE_WAIT_ACK && flashOK && sendingFromQueue) {
                    flashQueue.pop();
                    sendingFromQueue = false;
                }
            }
        }
        else if (pktType == PKT_HEALTH_REQ) {
            HealthPacket hp;
            memcpy(&hp, buf, sizeof(HealthPacket));

            if (hp.magic == MAGIC_BYTE && healthPacketVerifyCRC(hp)) {
                handleHealthCheck(hp.sourceId);
            }
        }
        /* Unknown type at this size → ignore */
    }
    /* If we receive a DataPacket-sized payload, ignore it (children don't receive data) */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HEALTH CHECK RESPONSE
 * ═══════════════════════════════════════════════════════════════════════════ */
void handleHealthCheck(uint8_t requesterId) {
    Serial.println(F("[HEALTH] Alive check — responding."));

    HealthPacket resp;
    resp.magic      = MAGIC_BYTE;
    resp.type       = PKT_HEALTH_RESP;
    resp.sourceId   = THIS_NODE_ID;
    resp.targetId   = requesterId;
    resp.aliveFlags = (1 << (THIS_NODE_ID - 1));
    resp.padding    = 0;
    healthPacketSetCRC(resp);

    digitalWrite(PIN_LED_LORA, HIGH);
    ledLoraTimer = millis();

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&resp, sizeof(HealthPacket));
    LoRa.endPacket();       // Sync TX (blocks ~50ms)
    LoRa.receive();         // Back to RX

    currentState = STATE_HEALTH_RESPOND;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PERSIST SEQUENCE COUNTER TO FLASH
 *  Saves every 10 increments to reduce flash wear.
 * ═══════════════════════════════════════════════════════════════════════════ */
void persistSequenceId() {
    if (flashOK && (sequenceCounter % 10 == 0)) {
        flashQueue.saveSequenceId(SEQ_ID_FLASH_SECTOR, sequenceCounter);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  I2C BUS RECOVERY
 *  If the ESP32 slave reboots mid-transfer (watchdog), the ATmega's
 *  Wire library can hang waiting for SCL. This toggles SCL 9 times
 *  to free any stuck slave, then reinitializes Wire.
 * ═══════════════════════════════════════════════════════════════════════════ */
void i2cBusRecovery() {
    Serial.println(F("[I2C] Bus recovery..."));
    Wire.end();

    /* SCL = A5 on ATmega328P */
    pinMode(A5, OUTPUT);
    for (uint8_t i = 0; i < 9; i++) {
        digitalWrite(A5, HIGH);
        delayMicroseconds(5);
        digitalWrite(A5, LOW);
        delayMicroseconds(5);
    }
    digitalWrite(A5, HIGH);
    pinMode(A5, INPUT_PULLUP);

    /* SDA = A4 — release */
    pinMode(A4, INPUT_PULLUP);

    Wire.begin();
    Serial.println(F("[I2C] Bus recovered."));
}
