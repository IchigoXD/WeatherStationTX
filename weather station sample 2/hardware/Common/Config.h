/*
 * ============================================================================
 *  Weather Station Network — Node Configuration
 * ============================================================================
 *  Edit THIS_NODE_ID before compiling for each physical station.
 *  All other defaults should work for the standard hardware layout.
 *
 *  BUILD TIP: You can override THIS_NODE_ID from the compiler command line:
 *    -DTHIS_NODE_ID=NODE_CHILD_2
 *  This avoids accidental node ID mismatches.
 * ============================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "Protocol.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * >>> CHANGE THIS FOR EACH NODE  <<<
 * Set to NODE_CHILD_1, NODE_CHILD_2, NODE_CHILD_3, NODE_CHILD_4,
 *        NODE_PARENT, or NODE_MAIN
 *
 * WARNING: Flashing the wrong node ID causes TDMA collisions, ACK confusion,
 *          and wrong data attribution in the database!
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef THIS_NODE_ID
  #define THIS_NODE_ID    NODE_CHILD_1
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * >> LoRa PA Output Pin — MUST MATCH YOUR HARDWARE  <<
 *
 *  This project uses SX1278 RA-02 AI Thinker modules (433 MHz).
 *  The RA-02 routes its antenna to the PA_BOOST pin for +20dBm output.
 *  Using RFO on these modules will result in very poor range.
 *
 *  PA_OUTPUT_PA_BOOST_PIN — Use if your LoRa module's antenna is connected
 *                           to the PA_BOOST pin. Common on:
 *                           - SX1278 RA-02 AI Thinker (this project)
 *                           - SX1276MB1LAS shields
 *                           - Dragino LoRa HAT
 *                           - Red-labelled "SX1276" modules
 *
 *  PA_OUTPUT_RFO_PIN      — Use if your antenna is connected to the RFO
 *                           pin. Common on:
 *                           - Standard blue-labelled SX1278 433 MHz modules
 *                           ** Using PA_BOOST on RFO-wired modules will
 *                              produce near-zero output and may damage
 *                              the power amplifier! **
 *
 *  HOW TO CHECK: Look at the SMA/IPEX connector on your board. If a trace
 *  runs from it to the pin labelled "PA_BOOST" on the IC, use PA_BOOST.
 *  If the trace goes to "RFO_HF", use RFO.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define LORA_PA_PIN     PA_OUTPUT_PA_BOOST_PIN

/* When using RFO pin, max TX power is 14 dBm (not 20) */
#if LORA_PA_PIN == PA_OUTPUT_RFO_PIN
  #undef  LORA_TX_POWER
  #define LORA_TX_POWER  14
#endif

/* ── Pin Assignments (ATmega328P) ──────────────────────────────────────── */
#define PIN_SECURITY    2           // Security tamper detection (INT0-capable)
#define PIN_LORA_DIO0   3           // LoRa interrupt (INT1-capable)
#define PIN_SIREN_RELAY 4           // Siren relay (parent only)
#define PIN_FLASH_CS    8           // W25Q64 SPI flash chip select
#define PIN_LORA_RST    9           // LoRa reset
#define PIN_LORA_CS     10          // LoRa chip select (SPI SS)

// Status LEDs
#define PIN_LED_LORA    A2          // LED to indicate LoRa communication
#define PIN_LED_GSM     A3          // LED to indicate GSM communication

/* ── I2C Addresses ─────────────────────────────────────────────────────── */
#define I2C_ESP32_SLAVE 0x08        // ESP32 sensor slave

/* ── Timing (milliseconds) ─────────────────────────────────────────────── */
#define SENSOR_READ_INTERVAL    120000UL    // 2 minutes between readings
#define ACK_TIMEOUT_MS          3000UL      // Wait for ACK before retry
#define MAX_RETRIES             3           // Max retransmission attempts (reduced from 5 to fit TDMA slot)
#define RETRY_BACKOFF_BASE_MS   1500UL      // Base backoff (doubles each retry)
#define QUEUE_CHECK_INTERVAL    15000UL     // Check memory queue every 15 seconds
#define GPRS_POLL_INTERVAL      2000UL      // GPRS queue check interval (Main)
#define HEALTH_CHECK_TIMEOUT    10000UL     // Time to collect health responses
#define GPRS_RECONNECT_INTERVAL 30000UL     // GPRS reconnection cooldown

/* ── SIM800A (Main Station only) ───────────────────────────────────────── */
#define SIM_APN         "airtelgprs.com"

/* ═══════════════════════════════════════════════════════════════════════════
 * >>> SERVER ADDRESS — CHANGE TO YOUR PC's LAN IP  <<<
 * Find your IP:  Windows → ipconfig  |  Linux → ip addr
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SERVER_URL      "http://light-eyes-attend.loca.lt/upload"
#define SERVER_HEALTH   "http://light-eyes-attend.loca.lt/api/health_response"

/* ── Memory Safety ─────────────────────────────────────────────────────── */
/*
 * ATmega328P has 2048 bytes of SRAM.
 * W25Q64 flash queue is external (SPI). No SD.h needed (+512 bytes free).
 * DataPacket is ~52 bytes.
 * Parent relay queue reduced to 3 entries to save 92 bytes of SRAM.
 */
#define PARENT_RELAY_QUEUE_SIZE 3   // In-RAM relay buffer for parent node

/* ── Sequence ID Flash Address ─────────────────────────────────────────── */
/*
 * The last-used sequence counter is persisted to a dedicated flash sector
 * so that it survives reboots and avoids server dedup collisions.
 * Uses sector 2046 of the W25Q64 (far from the queue data sectors 1..2045).
 */
#define SEQ_ID_FLASH_SECTOR     2046

/* ── Debug Mode (Main Station only) ────────────────────────────────────── */
/*
 * SoftwareSerial on Main Station disables interrupts during TX.
 * This can cause LoRa DIO0 misses and SPI corruption.
 * DISABLE for production by commenting out the line below.
 */
// #define ENABLE_DEBUG

/* ── LoRa Setup Helper ─────────────────────────────────────────────────── */
/*
 * Call this after LoRa.begin() to apply full network configuration.
 * Ensures maximum range and consistent settings across all nodes.
 */
static inline void configureLora() {
    /*
     * PA pin MUST match your hardware wiring.
     * Set LORA_PA_PIN in Config.h. Using the wrong one risks damage.
     * See Config.h for detailed instructions on how to identify your module.
     * 
     * RA-02 AI Thinker modules REQUIRE PA_OUTPUT_PA_BOOST_PIN for +20dBm.
     */
    LoRa.setTxPower(LORA_TX_POWER, LORA_PA_PIN);
    LoRa.setSignalBandwidth(LORA_BANDWIDTH);
    LoRa.setSpreadingFactor(LORA_SPREAD_FACTOR);
    LoRa.setCodingRate4(LORA_CODING_RATE);
    LoRa.setSyncWord(MAGIC_BYTE);
    LoRa.enableCrc();   // Hardware-level CRC (additional layer)
}

#endif // CONFIG_H
