/*
 * ============================================================================
 *  Weather Station Network — Main Station Firmware
 * ============================================================================
 *  Hardware: ATmega328P + LoRa + SIM800A (pins 0,1) + W25Q64 + RTC
 *
 *  Responsibilities:
 *    1. Receive all data packets from Parent via LoRa
 *    2. Send ACK back to Parent
 *    3. Store received packets to flash queue
 *    4. Upload packets to server via SIM800A GPRS (HTTP POST)
 *    5. Parse +HTTPACTION: URC to confirm delivery
 *    6. Handle health-check requests from server
 *    7. Forward health-check to Parent, collect responses, reply to server
 *
 *  Architecture: Fully non-blocking. NO delay() calls.
 *    - SIM800A uses non-blocking AT command state machine
 *    - Multi-line AT response ring buffer to prevent URC loss
 *    - Waits for DOWNLOAD prompt before sending HTTP data
 *    - LoRa continuously monitored even during GPRS operations
 *    - AVR watchdog timer (8-second timeout)
 *    - Async LoRa TX via endPacket(true)
 *    - Debug output is conditional (#define ENABLE_DEBUG in Config.h)
 * ============================================================================
 */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include "RTClib.h"
#include <avr/wdt.h>
#include "E:\weather station sample 2\hardware\Common\Config.h"
#include "E:\weather station sample 2\hardware\Common\FlashQueue.h"

#define MY_NODE_ID   NODE_MAIN

/* MS-03: Compile-time check — SERVER_URL must fit in AT command buffer (100 bytes) */
/* AT+HTTPPARA="URL","..." adds ~22 chars overhead + null terminator */
#if defined(__cplusplus)
static_assert(sizeof(SERVER_URL) < 76, "SERVER_URL too long for 100-byte AT command buffer");
#endif

/* ── Conditional Debug Serial ──────────────────────────────────────────── */
#ifdef ENABLE_DEBUG
//  #include <SoftwareSerial.h>
//  #define DEBUG_RX 5
//  #define DEBUG_TX 6
//  SoftwareSerial debugSerial(DEBUG_RX, DEBUG_TX);
  #define DBG(x)    Serial.print(x)
  #define DBGLN(x)  Serial.println(x)
  #define DBGF(x)   Serial.print(F(x))
  #define DBGFLN(x) Serial.println(F(x))
#else
  #define DBG(x)    ((void)0)
  #define DBGLN(x)  ((void)0)
  #define DBGF(x)   ((void)0)
  #define DBGFLN(x) ((void)0)
#endif

/* ── Multi-line AT Response Buffer ─────────────────────────────────────── */
/*
 * The SIM800A sends multi-line responses. We need to capture specific URCs
 * like +HTTPACTION: separately from generic OK/ERROR responses.
 * atLineBuf stores the current line being received.
 * When a complete line is received, it is checked and stored.
 *
 * httpActionBuf specifically captures the +HTTPACTION: URC line.
 */
#define AT_LINE_BUF_SIZE  80
#define AT_SPECIAL_BUF_SIZE 40

char atLineBuf[AT_LINE_BUF_SIZE];
uint8_t atLineIdx = 0;
bool atResponseReady = false;         // Generic response line is ready

char httpActionBuf[AT_SPECIAL_BUF_SIZE]; // Captures +HTTPACTION: URC
bool httpActionReady = false;

char httpReadBuf[AT_LINE_BUF_SIZE];       // Captures HTTP response body
bool httpReadReady = false;

bool downloadPromptReady = false;     // Set when DOWNLOAD\r\n is received

/* ── GPRS State Machine ───────────────────────────────────────────────── */
enum GprsState : uint8_t {
    GPRS_BOOT,
    GPRS_INIT_AT,
    GPRS_INIT_CREG,
    GPRS_INIT_CREG_WAIT,
    GPRS_INIT_CONTYPE,
    GPRS_INIT_APN,
    GPRS_INIT_OPEN,
    GPRS_INIT_OPEN_WAIT,
    GPRS_READY,
    GPRS_HTTP_INIT,
    GPRS_HTTP_CID,
    GPRS_HTTP_URL,
    GPRS_HTTP_CONTENT,
    GPRS_HTTP_DATA_CMD,
    GPRS_HTTP_DATA_WAIT_DL,     // Wait for DOWNLOAD prompt
    GPRS_HTTP_DATA_SEND,
    GPRS_HTTP_ACTION,
    GPRS_HTTP_WAIT_RESP,
    GPRS_HTTP_READ_CMD,
    GPRS_HTTP_READ_WAIT,
    GPRS_HTTP_TERM,
    GPRS_UPLOAD_SUCCESS,
    GPRS_UPLOAD_FAIL,
    GPRS_CHECK_SIGNAL,
    GPRS_HEALTH_INIT,
    GPRS_HEALTH_CID,
    GPRS_HEALTH_URL,
    GPRS_HEALTH_ACTION,
    GPRS_HEALTH_WAIT_RESP,
    GPRS_HEALTH_TERM,
    GPRS_ERROR
};

GprsState gprsState          = GPRS_BOOT;
unsigned long gprsTimer      = 0;
unsigned long gprsTimeout    = 0;

/* ── Globals ───────────────────────────────────────────────────────────── */
uint32_t ledLoraTimer = 0;
uint32_t ledGsmTimer  = 0;

RTC_DS3231 rtc;
FlashQueue flashQueue;
bool rtcAvailable            = false;
bool flashOK                 = false;
unsigned long lastSignalCheck = 0;

/* Current upload packet */
DataPacket uploadPacket;
bool uploadReady             = false;

/* Health check */
bool healthPending           = false;
bool healthResponseReady     = false;
uint8_t healthFlags          = 0;
unsigned long healthTimer    = 0;

/* ── Forward Declarations ──────────────────────────────────────────────── */
void handleLoRa();
void processGprs();
void sendATCommand(const char* cmd);
void sendATCommand_P(const __FlashStringHelper* cmd);
bool parseHttpActionOK();
void readATResponse();
void sendAckToParent(uint8_t origSrcId, uint16_t seqId);
void sendHealthCheckToParent();    // Added missing forward declaration
void clearATBuffers();

/* ═══════════════════════════════════════════════════════════════════════════
 *  SETUP
 * ═══════════════════════════════════════════════════════════════════════════ */
void setup() {
    wdt_disable();

    /* SIM800A on hardware serial (pins 0,1) */
    Serial.begin(9600);

    #ifdef ENABLE_DEBUG
//    debugSerial.begin(9600);
    #endif

    DBGFLN("[Main] Initializing...");

    /* RTC */
    Wire.begin();
    rtcAvailable = rtc.begin();
    DBG(F("  RTC: "));
    DBGLN(rtcAvailable ? F("OK") : F("NOT FOUND"));

    /* Flash Queue (W25Q64) */
    SPI.begin();
    
    /* Status LEDs */
    pinMode(PIN_LED_LORA, OUTPUT);
    pinMode(PIN_LED_GSM, OUTPUT);
    
    /* LED Boot Test — if they don't turn on here, the wiring is wrong! */
    digitalWrite(PIN_LED_LORA, HIGH);
    digitalWrite(PIN_LED_GSM, HIGH);
    { unsigned long t = millis(); while (millis() - t < 500) { wdt_reset(); } }
    digitalWrite(PIN_LED_LORA, LOW);
    digitalWrite(PIN_LED_GSM, LOW);

    flashOK = flashQueue.begin(PIN_FLASH_CS, sizeof(DataPacket), PIN_LORA_CS);
    DBG(F("  SPI Flash: "));
    if (flashOK) {
        DBG(F("OK  ("));
        DBG(flashQueue.available());
        DBG(F("/"));
        DBG(flashQueue.maxEntries);
        DBGLN(F(" queued)"));
    } else {
        DBGLN(F("NOT FOUND"));
    }

    /* LoRa */
    LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);
    if (!LoRa.begin(LORA_FREQ)) {
        DBGFLN("  LoRa: FAILED — halting");
        while (1) { wdt_reset(); }
    }
    configureLora();
    DBGFLN("  LoRa: OK");

    /* Start GPRS boot sequence */
    gprsState = GPRS_BOOT;
    gprsTimer = millis();

    /* Clear AT buffers */
    clearATBuffers();

    DBGFLN("[Main] Ready.\n");

    wdt_enable(WDTO_8S);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MAIN LOOP — Dual state machines (LoRa + GPRS) running concurrently
 * ═══════════════════════════════════════════════════════════════════════════ */
void loop() {
    wdt_reset();
    handleLoRa();
    readATResponse();
    processGprs();

    /* Non-blocking LED timeout (200ms flash) */
    if (ledLoraTimer > 0 && millis() - ledLoraTimer > 200) {
        digitalWrite(PIN_LED_LORA, LOW);
        ledLoraTimer = 0;
    }
    if (ledGsmTimer > 0 && millis() - ledGsmTimer > 200) {
        digitalWrite(PIN_LED_GSM, LOW);
        ledGsmTimer = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LoRa RECEIVE HANDLER
 * ═══════════════════════════════════════════════════════════════════════════ */
void handleLoRa() {
    int packetSize = LoRa.parsePacket();
    if (packetSize == 0) return;

    /* ── Data Packet (52 bytes total with __attribute__((packed))) ──────────── */
    if (packetSize == sizeof(DataPacket)) {
        DataPacket received;
        LoRa.readBytes((uint8_t*)&received, sizeof(DataPacket));

        if (received.magic != MAGIC_BYTE || !dataPacketVerifyCRC(received)) {
            DBGFLN("[RX] Bad CRC — dropped.");
            return;
        }

        if (received.type == PKT_DATA) {
            DBG(F("[RX] node="));
            DBG(received.sourceId);
            DBG(F(" seq="));
            DBGLN(received.sequenceId);

            /* Flash LoRa LED */
            digitalWrite(PIN_LED_LORA, HIGH);
            ledLoraTimer = millis();

            /* If packet came from Parent directly (rssi=0), capture Parent-Main quality.
             * If it's a relayed child packet (rssi!=0), it already contains Child-Parent quality. */
            if (received.rssi == 0) {
                received.rssi = LoRa.packetRssi();
                received.snr  = LoRa.packetSnr();
                dataPacketSetCRC(received); // Update CRC for server validation
            }

            if (flashOK) {
                flashQueue.push((const uint8_t*)&received);
            } else {
                /* RAM Fallback if SPI Flash is broken: Buffer the latest packet */
                memcpy(&uploadPacket, &received, sizeof(DataPacket));
                uploadReady = true;
            }
            sendAckToParent(received.sourceId, received.sequenceId);
        }
    }
    /* ─── Health Response from Parent (8 bytes) ─── */
    else if (packetSize == sizeof(HealthPacket)) {
        uint8_t buf[sizeof(HealthPacket)];
        LoRa.readBytes(buf, sizeof(HealthPacket));

        uint8_t pktType = buf[1];

        if (pktType == PKT_HEALTH_RESP) {
            HealthPacket hp;
            memcpy(&hp, buf, sizeof(HealthPacket));

            if (hp.magic == MAGIC_BYTE && healthPacketVerifyCRC(hp)) {
                /* Flash LoRa LED */
                digitalWrite(PIN_LED_LORA, HIGH);
                ledLoraTimer = millis();

                healthFlags = hp.aliveFlags;
                healthResponseReady = true;
                DBG(F("[HEALTH] flags=0x"));
                DBGLN(healthFlags);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEND ACK TO PARENT (async TX)
 * ═══════════════════════════════════════════════════════════════════════════ */
void sendAckToParent(uint8_t origSrcId, uint16_t seqId) {
    digitalWrite(PIN_LED_LORA, HIGH);
    ledLoraTimer = millis();

    AckPacket ack;
    ack.magic      = MAGIC_BYTE;
    ack.type       = PKT_ACK;
    ack.sourceId   = MY_NODE_ID;
    ack.targetId   = NODE_PARENT;
    ack.sequenceId = seqId;
    ackPacketSetCRC(ack);

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&ack, sizeof(AckPacket));
    LoRa.endPacket();       // Sync TX (blocks ~50ms)
    LoRa.receive();         // Back to continuous RX

    DBG(F("[ACK→Parent] seq="));
    DBGLN(seqId);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GPRS STATE MACHINE — Fully non-blocking
 * ═══════════════════════════════════════════════════════════════════════════ */
void processGprs() {
    unsigned long now = millis();

    switch (gprsState) {

        case GPRS_BOOT:
            if (now - gprsTimer >= 2000) {
                gprsState = GPRS_INIT_AT;
                gprsTimer = now;
            }
            break;

        case GPRS_INIT_AT:
            sendATCommand_P(F("AT"));
            gprsState = GPRS_INIT_CREG;
            gprsTimer = now;
            gprsTimeout = 2000;
            break;

        case GPRS_INIT_CREG:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand_P(F("AT+CREG?"));
                gprsState = GPRS_INIT_CREG_WAIT;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_INIT_CREG_WAIT:
            if (atResponseReady) {
                if (strstr(atLineBuf, "+CREG: 0,1") || strstr(atLineBuf, "+CREG: 0,5")) {
                    gprsState = GPRS_INIT_CONTYPE;
                    DBGFLN("[GPRS] Network Registered.");
                } else if (strstr(atLineBuf, "ERROR")) {
                    gprsState = GPRS_INIT_CREG;
                }
                clearATBuffers();
            } else if (now - gprsTimer >= gprsTimeout) {
                gprsState = GPRS_INIT_CREG;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_INIT_CONTYPE:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand_P(F("AT+SAPBR=3,1,\"Contype\",\"GPRS\""));
                gprsState = GPRS_INIT_APN;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_INIT_APN:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand("AT+SAPBR=3,1,\"APN\",\"" SIM_APN "\"");
                gprsState = GPRS_INIT_OPEN;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_INIT_OPEN:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand_P(F("AT+SAPBR=1,1"));
                gprsState = GPRS_INIT_OPEN_WAIT;
                gprsTimer = now;
                gprsTimeout = 5000;
                clearATBuffers();
            }
            break;

        case GPRS_INIT_OPEN_WAIT:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                gprsState = GPRS_READY;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        /* ── Ready / Idle — Check for work ── */
        case GPRS_READY:
            if (now - gprsTimer < gprsTimeout) break;

            /* Priority 1: Health check response to upload */
            if (healthResponseReady) {
                healthResponseReady = false;
                uploadReady = false;  // Ensure data upload flag is clear for health flow
                gprsState = GPRS_HEALTH_INIT;
                gprsTimer = now;
                break;
            }

            /* Priority 2: Data packets to upload */
            if (flashOK && !flashQueue.isEmpty()) {
                if (flashQueue.peek((uint8_t*)&uploadPacket)) {
                    uploadReady = true;
                    gprsState = GPRS_HTTP_INIT;
                    gprsTimer = now;
                    DBGFLN("[GPRS] Starting upload (from Flash)...");
                }
            } else if (!flashOK && uploadReady) {
                gprsState = GPRS_HTTP_INIT;
                gprsTimer = now;
                DBGFLN("[GPRS] Starting upload (RAM fallback)...");
            }

            /* Priority 3: Periodic signal check */
            if (gprsState == GPRS_READY && now - lastSignalCheck >= 60000UL) {
                lastSignalCheck = now;
                sendATCommand_P(F("AT+CSQ"));
                gprsState = GPRS_CHECK_SIGNAL;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_CHECK_SIGNAL:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                if (atResponseReady) {
                    DBG(F("[SIGNAL] "));
                    DBGLN(atLineBuf);
                }
                gprsState = GPRS_READY;
                gprsTimer = now;
                gprsTimeout = 0;
                clearATBuffers();
            }
            break;

        /* ── HTTP Upload Sequence ── */
        case GPRS_HTTP_INIT:
            sendATCommand_P(F("AT+HTTPINIT"));
            gprsState = GPRS_HTTP_CID;
            gprsTimer = now;
            gprsTimeout = 2000;
            clearATBuffers();
            break;

        case GPRS_HTTP_CID:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand_P(F("AT+HTTPPARA=\"CID\",1"));
                gprsState = GPRS_HTTP_URL;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_HTTP_URL: {
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                char cmd[100];
                snprintf_P(cmd, sizeof(cmd), PSTR("AT+HTTPPARA=\"URL\",\"%s\""), SERVER_URL);
                sendATCommand(cmd);
                gprsState = GPRS_HTTP_CONTENT;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;
        }

        case GPRS_HTTP_CONTENT:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand_P(F("AT+HTTPPARA=\"CONTENT\",\"application/octet-stream\""));
                gprsState = GPRS_HTTP_DATA_CMD;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_HTTP_DATA_CMD: {
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                char cmd[40];
                snprintf_P(cmd, sizeof(cmd), PSTR("AT+HTTPDATA=%u,5000"), (unsigned)sizeof(DataPacket));
                sendATCommand(cmd);
                downloadPromptReady = false;
                gprsState = GPRS_HTTP_DATA_WAIT_DL;
                gprsTimer = now;
                gprsTimeout = 5000;  // SIM800A has up to 5 seconds
                clearATBuffers();
            }
            break;
        }

        /* ── Wait for DOWNLOAD prompt before sending data ── */
        case GPRS_HTTP_DATA_WAIT_DL:
            if (downloadPromptReady) {
                downloadPromptReady = false;
                gprsState = GPRS_HTTP_DATA_SEND;
                gprsTimer = now;
            } else if (now - gprsTimer >= gprsTimeout) {
                DBGFLN("[GPRS] DOWNLOAD timeout.");
                gprsState = GPRS_UPLOAD_FAIL;
                gprsTimer = now;
            }
            break;

        case GPRS_HTTP_DATA_SEND:
            /* Flash GSM LED */
            digitalWrite(PIN_LED_GSM, HIGH);
            ledGsmTimer = millis();

            Serial.write((uint8_t*)&uploadPacket, sizeof(DataPacket));
            gprsState = GPRS_HTTP_ACTION;
            gprsTimer = now;
            gprsTimeout = 2000;
            clearATBuffers();
            break;

        case GPRS_HTTP_ACTION:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand_P(F("AT+HTTPACTION=1"));
                httpActionReady = false;
                gprsState = GPRS_HTTP_WAIT_RESP;
                gprsTimer = now;
                gprsTimeout = 15000;
                clearATBuffers();
            }
            break;

        case GPRS_HTTP_WAIT_RESP:
            /*
             * Wait specifically for +HTTPACTION: URC in the dedicated buffer.
             * This prevents false matches from OK or signal quality lines.
             */
            if (httpActionReady) {
                if (parseHttpActionOK()) {
                    DBGFLN("[GPRS] HTTP 200 — OK. Reading response body...");
                    gprsState = GPRS_HTTP_READ_CMD;
                } else {
                    DBGFLN("[GPRS] HTTP failed.");
                    gprsState = GPRS_UPLOAD_FAIL;
                }
                gprsTimer = now;
                gprsTimeout = 2000;
                httpActionReady = false;
            }
            else if (now - gprsTimer >= gprsTimeout) {
                DBGFLN("[GPRS] HTTP timeout.");
                gprsState = GPRS_UPLOAD_FAIL;
                gprsTimer = now;
            }
            break;

        case GPRS_HTTP_READ_CMD:
            sendATCommand_P(F("AT+HTTPREAD"));
            httpReadReady = false;
            gprsState = GPRS_HTTP_READ_WAIT;
            gprsTimer = now;
            gprsTimeout = 3000;
            clearATBuffers();
            break;

        case GPRS_HTTP_READ_WAIT:
            if (httpReadReady || atResponseReady) {
                /* Check for health check command in response body */
                char* searchBuf = httpReadReady ? httpReadBuf : atLineBuf;
                if (strstr(searchBuf, "CHECK_ALIVE") != NULL) {
                    DBGFLN("[GPRS] Server requested health check! Triggering...");
                    sendHealthCheckToParent();
                }
                gprsState = GPRS_HTTP_TERM;
                gprsTimer = now;
                gprsTimeout = 1000;
                clearATBuffers();
            } else if (now - gprsTimer >= gprsTimeout) {
                gprsState = GPRS_HTTP_TERM;
                gprsTimer = now;
                gprsTimeout = 1000;
                clearATBuffers();
            }
            break;

        case GPRS_HTTP_TERM:
            sendATCommand_P(F("AT+HTTPTERM"));
            gprsState = GPRS_UPLOAD_SUCCESS;
            gprsTimer = now;
            gprsTimeout = 1000;
            clearATBuffers();
            break;

        case GPRS_UPLOAD_SUCCESS:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                /* Pop the uploaded packet from Flash queue */
                if (flashOK && uploadReady) {
                    flashQueue.pop();
                    DBGFLN("[GPRS] Packet uploaded + removed.");
                } else {
                    DBGFLN("[GPRS] Request succeeded.");
                }
                uploadReady = false;

                gprsState = GPRS_READY;
                gprsTimer = now;
                gprsTimeout = 0;
                clearATBuffers();
            }
            break;

        case GPRS_UPLOAD_FAIL:
            digitalWrite(PIN_LED_GSM, HIGH);
            ledGsmTimer = millis();

            sendATCommand_P(F("AT+HTTPTERM"));
            uploadReady = false;
            gprsState = GPRS_READY;
            gprsTimer = now;
            gprsTimeout = GPRS_RECONNECT_INTERVAL;
            clearATBuffers();
            DBGFLN("[GPRS] Retry in 30s.");
            break;

        /* ── Health Check Upload (separate path, never pops data queue) ── */
        case GPRS_HEALTH_INIT:
            sendATCommand_P(F("AT+HTTPINIT"));
            gprsState = GPRS_HEALTH_CID;
            gprsTimer = now;
            gprsTimeout = 2000;
            clearATBuffers();
            break;

        case GPRS_HEALTH_CID:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                sendATCommand_P(F("AT+HTTPPARA=\"CID\",1"));
                gprsState = GPRS_HEALTH_URL;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_HEALTH_URL:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                char cmd[100];
                snprintf_P(cmd, sizeof(cmd), PSTR("AT+HTTPPARA=\"URL\",\"%s?flags=%u\""),
                           SERVER_HEALTH, (unsigned)healthFlags);
                sendATCommand(cmd);
                gprsState = GPRS_HEALTH_ACTION;
                gprsTimer = now;
                gprsTimeout = 2000;
                clearATBuffers();
            }
            break;

        case GPRS_HEALTH_ACTION:
            if (atResponseReady || now - gprsTimer >= gprsTimeout) {
                /* Flash GSM LED */
                digitalWrite(PIN_LED_GSM, HIGH);
                ledGsmTimer = millis();

                sendATCommand_P(F("AT+HTTPACTION=0"));  // GET
                httpActionReady = false;
                gprsState = GPRS_HEALTH_WAIT_RESP;
                gprsTimer = now;
                gprsTimeout = 10000;
                clearATBuffers();
                healthPending = false;
            }
            break;

        /* Health response handler — separate from data upload */
        case GPRS_HEALTH_WAIT_RESP:
            if (httpActionReady || now - gprsTimer >= gprsTimeout) {
                gprsState = GPRS_HEALTH_TERM;
                gprsTimer = now;
                gprsTimeout = 1000;
                httpActionReady = false;
            }
            break;

        case GPRS_HEALTH_TERM:
            if (now - gprsTimer >= gprsTimeout || atResponseReady) {
                sendATCommand_P(F("AT+HTTPTERM"));
                /* uploadReady is already false — no data queue pop happens */
                gprsState = GPRS_READY;
                gprsTimer = now;
                gprsTimeout = 1000;
                clearATBuffers();
            }
            break;

        case GPRS_ERROR:
            if (now - gprsTimer >= GPRS_RECONNECT_INTERVAL) {
                DBGFLN("[GPRS] Reconnecting...");
                gprsState = GPRS_INIT_AT;
                gprsTimer = now;
            }
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AT COMMAND HELPERS — Non-blocking
 * ═══════════════════════════════════════════════════════════════════════════ */
void sendATCommand(const char* cmd) {
    Serial.println(cmd);
    DBG(F(">> ")); DBGLN(cmd);
    clearATBuffers();
}

void sendATCommand_P(const __FlashStringHelper* cmd) {
    Serial.println(cmd);
    DBG(F(">> ")); DBGLN(cmd);
    clearATBuffers();
}

void clearATBuffers() {
    atLineIdx = 0;
    atResponseReady = false;
    memset(atLineBuf, 0, AT_LINE_BUF_SIZE);
}

/*
 * Called every loop() — reads available Serial bytes without blocking.
 * Routes special lines to dedicated buffers:
 *   - "+HTTPACTION:" → httpActionBuf / httpActionReady
 *   - "DOWNLOAD"     → downloadPromptReady
 *   - Data lines (CHECK_ALIVE, etc.) → httpReadBuf / httpReadReady
 *   - All other complete lines → atLineBuf / atResponseReady
 */
void readATResponse() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            if (atLineIdx > 0) {
                atLineBuf[atLineIdx] = '\0';
                DBG(F("<< ")); DBGLN(atLineBuf);

                /* Route to specific buffers based on content */
                if (strstr(atLineBuf, "+HTTPACTION:") != NULL) {
                    strncpy(httpActionBuf, atLineBuf, AT_SPECIAL_BUF_SIZE - 1);
                    httpActionBuf[AT_SPECIAL_BUF_SIZE - 1] = '\0';
                    httpActionReady = true;
                }
                else if (strstr(atLineBuf, "DOWNLOAD") != NULL) {
                    downloadPromptReady = true;
                }
                else if (strstr(atLineBuf, "CHECK_ALIVE") != NULL ||
                         strstr(atLineBuf, "OK") != NULL ||
                         strstr(atLineBuf, "ERROR") != NULL) {
                    /* Check for response body content */
                    if (strstr(atLineBuf, "CHECK_ALIVE") != NULL) {
                        strncpy(httpReadBuf, atLineBuf, AT_LINE_BUF_SIZE - 1);
                        httpReadBuf[AT_LINE_BUF_SIZE - 1] = '\0';
                        httpReadReady = true;
                    }
                    atResponseReady = true;
                }
                else if (atLineIdx > 1) {
                    /* Generic response line */
                    atResponseReady = true;
                }
            }
            atLineIdx = 0;
        } else if (c != '\r' && atLineIdx < AT_LINE_BUF_SIZE - 1) {
            atLineBuf[atLineIdx++] = c;
        }
    }
}

/*
 * Parse +HTTPACTION: URC from the dedicated httpActionBuf.
 * Format: "+HTTPACTION: <method>,<status>,<datalen>"
 * Returns true only if status == 200.
 */
bool parseHttpActionOK() {
    char* p = strstr(httpActionBuf, "+HTTPACTION:");
    if (!p) return false;

    /* Find the first comma (after method) */
    p = strchr(p, ',');
    if (!p) return false;
    p++;  // skip comma

    /* Parse the status code */
    int status = atoi(p);
    return (status == 200);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HEALTH CHECK: Forward to LoRa network (async TX)
 * ═══════════════════════════════════════════════════════════════════════════ */
void sendHealthCheckToParent() {
    digitalWrite(PIN_LED_LORA, HIGH);
    ledLoraTimer = millis();

    HealthPacket req;
    req.magic      = MAGIC_BYTE;
    req.type       = PKT_HEALTH_REQ;
    req.sourceId   = MY_NODE_ID;
    req.targetId   = NODE_PARENT;
    req.aliveFlags = 0;
    req.padding    = 0;
    healthPacketSetCRC(req);

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&req, sizeof(HealthPacket));
    LoRa.endPacket();       // Sync TX (blocks ~50ms)
    LoRa.receive();         // Back to continuous RX

    healthPending = true;
    DBGFLN("[HEALTH] Sent to Parent.");
}
