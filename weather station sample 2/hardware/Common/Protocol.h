/*
 * ============================================================================
 *  Weather Station Network — Common Protocol Header
 * ============================================================================
 *  Shared data structures, packet formats, CRC16, and TDMA configuration
 *  used by all nodes in the network (Child, Parent, Main).
 *
 *  Packet Types:
 *    DATA           – Sensor readings from any node
 *    ACK            – Acknowledgement of a DATA packet
 *    HEALTH_REQ     – Server requests alive check via Main station
 *    HEALTH_RESP    – Node responds to alive check
 *
 *  Identity & Integrity:
 *    - MAGIC_BYTE sync word filters non-network LoRa traffic
 *    - CRC16 (CCITT) detects multi-bit corruption
 *    - nodeId in every packet prevents cross-node ACK confusion
 * ============================================================================
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <Arduino.h>

/* ── LoRa RF Configuration ─────────────────────────────────────────────── */
#define LORA_FREQ           433E6       // 433 MHz band
#define LORA_TX_POWER       20          // Max power (dBm) with PA_BOOST
#define LORA_BANDWIDTH      125E3       // 125 kHz — good range/speed balance
#define LORA_SPREAD_FACTOR  9           // SF9 — good range for rural stations
#define LORA_CODING_RATE    5           // 4/5 — adequate FEC, ~half the air time of 4/8
#define MAGIC_BYTE          0xAF        // Network sync word

/* ── Node Identifiers ──────────────────────────────────────────────────── */
enum NodeId : uint8_t {
    NODE_CHILD_1 = 1,
    NODE_CHILD_2 = 2,
    NODE_CHILD_3 = 3,
    NODE_CHILD_4 = 4,
    NODE_PARENT  = 5,
    NODE_MAIN    = 6,
    NODE_SERVER  = 7
};

/* ── Packet Types ──────────────────────────────────────────────────────── */
enum PacketType : uint8_t {
    PKT_DATA         = 0x01,
    PKT_ACK          = 0x02,
    PKT_HEALTH_REQ   = 0x03,
    PKT_HEALTH_RESP  = 0x04
};

/* ── TDMA Configuration ───────────────────────────────────────────────── */
/*
 * Each child has a 12-second transmission window within each 60-second
 * TDMA cycle. The parent transmits in its own slot after all children.
 *
 *   Child 1: 0–12 sec
 *   Child 2: 12–24 sec
 *   Child 3: 24–36 sec
 *   Child 4: 36–48 sec
 *   Parent:  48–60 sec
 *
 * Nodes calculate their slot from millis() modulo TDMA_CYCLE_MS.
 */
#define TDMA_CYCLE_MS       60000UL     // 60-second cycle
#define TDMA_SLOT_MS        12000UL     // 12 seconds per slot
#define TDMA_GUARD_MS       500UL       // 500 ms guard between slots

/* Returns the start of a node's TDMA slot within a cycle (ms) */
static inline unsigned long tdmaSlotStart(uint8_t nodeId) {
    /* nodeId 1–4 map to slots 0–3 ; nodeId 5 (parent) maps to slot 4 */
    uint8_t slot = (nodeId <= NODE_CHILD_4) ? (nodeId - 1) : 4;
    return (unsigned long)slot * TDMA_SLOT_MS;
}

/* Check if now (cyclePos = millis() % TDMA_CYCLE_MS) is inside this node's slot */
static inline bool tdmaIsMySlot(uint8_t nodeId, unsigned long cyclePos) {
    unsigned long start = tdmaSlotStart(nodeId);
    unsigned long end   = start + TDMA_SLOT_MS - TDMA_GUARD_MS;
    return (cyclePos >= start && cyclePos < end);
}

/* ── Sensor Data (shared between all node types) ───────────────────────── */
struct SensorData {
    float    temp;              // AHT10 temperature (°C)
    float    humidity;          // AHT10 relative humidity (%)
    float    soil_temp[5];      // DS18B20 depths: 0m, 20m, 40m, 60m, 80m
    uint16_t leaf_wetness;      // Capacitive leaf sensor ADC value
    uint16_t rain_gauge;        // Tipping bucket count (parent only, 0 for children)
    uint8_t  security_alert;    // 1 = security pin disconnected
    uint8_t  sensor_status;     // Bitmask: bit0=AHT10, bit1=DS18B20, bit2=leaf, bit3=rain
} __attribute__((packed));

/* ── Data Packet (52 bytes total with __attribute__((packed))) ──────────── */
struct DataPacket {
    uint8_t  magic;             // MAGIC_BYTE
    uint8_t  type;              // PacketType
    uint8_t  sourceId;          // Originating node ID
    uint8_t  reserved;          // Alignment / future use
    uint16_t sequenceId;        // Monotonically increasing per node
    uint32_t timestamp;         // Unix timestamp from RTC
    SensorData data;            // Sensor payload
    int16_t  rssi;              // LoRa signal strength (dBm)
    float    snr;               // Signal-to-noise ratio (dB)
    uint16_t crc;               // CRC16-CCITT over everything before this field
} __attribute__((packed));

/* ── ACK Packet ────────────────────────────────────────────────────────── */
struct AckPacket {
    uint8_t  magic;             // MAGIC_BYTE
    uint8_t  type;              // PKT_ACK
    uint8_t  sourceId;          // Who is sending the ACK
    uint8_t  targetId;          // Who the ACK is for
    uint16_t sequenceId;        // Which packet is being acknowledged
    uint16_t crc;               // CRC16
} __attribute__((packed));

/* ── Health Check Packet ───────────────────────────────────────────────── */
struct HealthPacket {
    uint8_t  magic;             // MAGIC_BYTE
    uint8_t  type;              // PKT_HEALTH_REQ or PKT_HEALTH_RESP
    uint8_t  sourceId;          // Requester or responder
    uint8_t  targetId;          // 0xFF = broadcast
    uint8_t  aliveFlags;        // Bitmask: bit0=child1 .. bit4=parent
    uint8_t  padding;
    uint16_t crc;               // CRC16
} __attribute__((packed));

/* ═══════════════════════════════════════════════════════════════════════════
 *  ESP32 Slave Payloads — unified definitions to prevent struct mismatch.
 *  The Child ESP32 sends SlavePayloadChild (24 bytes).
 *  The Parent ESP32 sends SlavePayloadParent (26 bytes, includes rain_count).
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Payload from child ESP32 slave (no rain gauge) — 24 bytes */
struct SlavePayloadChild {
    float    soil_temp[5];          // 5 depth probes (20 bytes)
    uint16_t leaf_wetness;          // ADC value (2 bytes)
    uint8_t  sensor_status;         // Bitmask: bit1=DS18B20 ok, bit2=leaf ok
    uint8_t  padding;               // Alignment
} __attribute__((packed));          // Total: 24 bytes

/* Payload from parent ESP32 slave (has rain gauge) — 26 bytes */
struct SlavePayloadParent {
    float    soil_temp[5];          // 5 depth probes (20 bytes)
    uint16_t leaf_wetness;          // ADC value (2 bytes)
    uint16_t rain_count;            // Tip count since last read (2 bytes)
    uint8_t  sensor_status;         // Bitmask
    uint8_t  padding;               // Alignment
} __attribute__((packed));          // Total: 26 bytes

/* ── Inter-sensor read delay (millis-based, used by ATmega state machines) */
#define SENSOR_READ_SUB_DELAY_MS  50UL  // 50ms between sensor reads

/* ── CRC16-CCITT (0xFFFF) ──────────────────────────────────────────────── */
static inline uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ── Packet Helpers ────────────────────────────────────────────────────── */

/* Calculate and set CRC for a DataPacket */
static inline void dataPacketSetCRC(DataPacket& p) {
    p.crc = crc16_ccitt((const uint8_t*)&p, sizeof(DataPacket) - sizeof(uint16_t));
}

/* Verify CRC of a DataPacket */
static inline bool dataPacketVerifyCRC(const DataPacket& p) {
    uint16_t expected = crc16_ccitt((const uint8_t*)&p, sizeof(DataPacket) - sizeof(uint16_t));
    return expected == p.crc;
}

/* Calculate and set CRC for an AckPacket */
static inline void ackPacketSetCRC(AckPacket& p) {
    p.crc = crc16_ccitt((const uint8_t*)&p, sizeof(AckPacket) - sizeof(uint16_t));
}

/* Verify CRC of an AckPacket */
static inline bool ackPacketVerifyCRC(const AckPacket& p) {
    uint16_t expected = crc16_ccitt((const uint8_t*)&p, sizeof(AckPacket) - sizeof(uint16_t));
    return expected == p.crc;
}

/* Calculate and set CRC for a HealthPacket */
static inline void healthPacketSetCRC(HealthPacket& p) {
    p.crc = crc16_ccitt((const uint8_t*)&p, sizeof(HealthPacket) - sizeof(uint16_t));
}

/* Verify CRC of a HealthPacket */
static inline bool healthPacketVerifyCRC(const HealthPacket& p) {
    uint16_t expected = crc16_ccitt((const uint8_t*)&p, sizeof(HealthPacket) - sizeof(uint16_t));
    return expected == p.crc;
}

#endif // PROTOCOL_H
