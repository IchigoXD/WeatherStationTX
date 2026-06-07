/*
 * ============================================================================
 *  AT24C256 I2C EEPROM — Circular Queue
 * ============================================================================
 *  Replaces SD card module. Uses external AT24C256 (32 KB) EEPROM.
 *
 *  EEPROM Memory Layout:
 *    Bytes 0-1:  head index       (uint16_t) — next entry to read
 *    Bytes 2-3:  tail index       (uint16_t) — next write position
 *    Bytes 4-5:  count            (uint16_t) — entries in queue
 *    Bytes 6-7:  init magic       (0xABCD)   — format validation
 *    Bytes 8+:   circular data buffer
 *
 *  Capacity: (32768 - 8) / entrySize slots
 *            For 52-byte DataPacket → 629 slots
 *
 *  Write Wear: Circular layout distributes writes across all cells.
 *              At 5 nodes × 144 writes/day → ~97,000 days per cell.
 *
 *  No delay() calls. EEPROM write completion uses ACK polling.
 * ============================================================================
 */

#ifndef EEPROM_QUEUE_H
#define EEPROM_QUEUE_H

#include <Arduino.h>
#include <Wire.h>

/* ── EEPROM Hardware Constants ─────────────────────────────────────────── */
#define EEPROM_I2C_ADDR      0x50       // A0=A1=A2=GND
#define EEPROM_TOTAL_SIZE    32768UL    // AT24C256 = 32 KB
#define EEPROM_HEADER_SIZE   8          // Queue metadata
#define EEPROM_MAGIC_INIT    0xABCD     // Indicates formatted EEPROM
#define EEPROM_PAGE_SIZE     64         // AT24C256 page boundary

class EEPROMQueue {
public:
    uint16_t entrySize;
    uint16_t maxEntries;

    /* Cached in RAM for fast access (synced to EEPROM on change) */
    uint16_t head;
    uint16_t tail;
    uint16_t count;

    /*
     * Initialize the queue. Must be called after Wire.begin().
     * Returns true if EEPROM was found and readable.
     */
    bool begin(uint16_t _entrySize) {
        entrySize = _entrySize;
        maxEntries = (uint16_t)((EEPROM_TOTAL_SIZE - EEPROM_HEADER_SIZE) / entrySize);

        /* Probe EEPROM presence */
        Wire.beginTransmission(EEPROM_I2C_ADDR);
        if (Wire.endTransmission() != 0) {
            return false;   // EEPROM not found on bus
        }

        /* Check if EEPROM has a valid formatted header */
        uint16_t magic = _readWord(6);
        if (magic == EEPROM_MAGIC_INIT) {
            head  = _readWord(0);
            tail  = _readWord(2);
            count = _readWord(4);
            /* Sanity check — corrupt pointers → reformat */
            if (head >= maxEntries || tail >= maxEntries || count > maxEntries) {
                format();
            }
        } else {
            format();   // First use — initialize
        }
        return true;
    }

    /* Clear the queue, reset all pointers to zero */
    void format() {
        head  = 0;
        tail  = 0;
        count = 0;
        _writeWord(0, head);
        _writeWord(2, tail);
        _writeWord(4, count);
        _writeWord(6, EEPROM_MAGIC_INIT);
    }

    /*
     * Push an entry onto the tail of the queue.
     * Returns false if queue is full (caller should log/discard).
     */
    bool push(const uint8_t* data) {
        if (count >= maxEntries) return false;

        uint32_t addr = EEPROM_HEADER_SIZE + (uint32_t)tail * entrySize;
        _writeBlock(addr, data, entrySize);

        tail = (tail + 1) % maxEntries;
        count++;

        _writeWord(2, tail);
        _writeWord(4, count);
        return true;
    }

    /*
     * Read the entry at the head without removing it.
     * Returns false if queue is empty.
     */
    bool peek(uint8_t* data) {
        if (count == 0) return false;

        uint32_t addr = EEPROM_HEADER_SIZE + (uint32_t)head * entrySize;
        _readBlock(addr, data, entrySize);
        return true;
    }

    /*
     * Remove the head entry (advance head pointer).
     * Call after a successful upload/relay.
     */
    bool pop() {
        if (count == 0) return false;

        head = (head + 1) % maxEntries;
        count--;

        _writeWord(0, head);
        _writeWord(4, count);
        return true;
    }

    uint16_t available() const { return count; }
    bool     isEmpty()   const { return count == 0; }
    bool     isFull()    const { return count >= maxEntries; }

private:
    /* ── 16-bit word read/write ────────────────────────────────────────── */

    uint16_t _readWord(uint16_t addr) {
        uint8_t buf[2];
        _readBlock((uint32_t)addr, buf, 2);
        return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }

    void _writeWord(uint16_t addr, uint16_t val) {
        uint8_t buf[2] = { (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
        _writeBlock((uint32_t)addr, buf, 2);
    }

    /* ── Block read (handles Wire 32-byte buffer limit) ────────────────── */

    void _readBlock(uint32_t addr, uint8_t* buf, uint16_t len) {
        uint16_t offset = 0;
        while (offset < len) {
            uint8_t chunk = (uint8_t)min((uint16_t)28, (uint16_t)(len - offset));
            uint16_t a = (uint16_t)(addr + offset);

            Wire.beginTransmission(EEPROM_I2C_ADDR);
            Wire.write((uint8_t)(a >> 8));
            Wire.write((uint8_t)(a & 0xFF));
            Wire.endTransmission();

            Wire.requestFrom((uint8_t)EEPROM_I2C_ADDR, chunk);
            for (uint8_t i = 0; i < chunk && Wire.available(); i++) {
                buf[offset + i] = Wire.read();
            }
            offset += chunk;
        }
    }

    /* ── Block write (handles page boundary + ACK polling) ─────────────── */

    void _writeBlock(uint32_t addr, const uint8_t* buf, uint16_t len) {
        uint16_t offset = 0;
        while (offset < len) {
            uint16_t a = (uint16_t)(addr + offset);

            /* Bytes remaining in current EEPROM page */
            uint8_t pageRemain = EEPROM_PAGE_SIZE - (uint8_t)(a % EEPROM_PAGE_SIZE);
            uint8_t chunk = (uint8_t)min((uint16_t)pageRemain, (uint16_t)(len - offset));
            chunk = min(chunk, (uint8_t)30);   // Wire TX buffer = 32 – 2 addr bytes

            Wire.beginTransmission(EEPROM_I2C_ADDR);
            Wire.write((uint8_t)(a >> 8));
            Wire.write((uint8_t)(a & 0xFF));
            Wire.write(buf + offset, chunk);
            Wire.endTransmission();

            _waitWriteComplete();

            offset += chunk;
        }
    }

    /*
     * ACK-polling wait for EEPROM write completion.
     * The EEPROM NACKs while an internal write is in progress.
     * Typically completes in 2–5 ms. Max wait: 10 ms.
     * NO delay() call — uses millis() bounded loop.
     */
    void _waitWriteComplete() {
        unsigned long start = millis();
        while (millis() - start < 10) {
            Wire.beginTransmission(EEPROM_I2C_ADDR);
            if (Wire.endTransmission() == 0) return; // Write done
        }
    }
};

#endif // EEPROM_QUEUE_H
