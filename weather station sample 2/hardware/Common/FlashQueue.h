/*
 * ============================================================================
 *  W25Q64 SPI Flash — Circular Queue
 * ============================================================================
 *  Replaces EEPROMQueue.h. Uses W25Q64 (8 MB SPI NOR Flash).
 *
 *  W25Q64 Key Specs:
 *    - 8,388,608 bytes (8 MB)  =  2048 sectors  ×  4096 bytes
 *    - SPI Mode 0 or 3, up to 80 MHz
 *    - Page Program: 256 bytes per page, ~3 ms max
 *    - Sector Erase: 4096 bytes, ~45 ms typical, 400 ms max
 *    - Erase required before rewriting any byte
 *    - Erased state = 0xFF
 *    - Write endurance: 100,000 erase cycles per sector
 *
 *  Queue Layout:
 *    Sector 0 (4096 bytes) — Metadata log (write-append, erase on full)
 *      Each metadata entry: 16 bytes (written sequentially)
 *      Capacity: 256 metadata writes before sector 0 needs erase
 *
 *    Sectors 1..2045 — Data circular buffer
 *      Each sector holds: floor(4096 / entrySize) entries
 *      For 52-byte DataPacket: 78 entries/sector × 2045 sectors = 159,510 max
 *
 *    Sector 2046 — Reserved for persistent sequence counter
 *    Sector 2047 — Reserved for future use
 *
 *  SPI Bus Sharing:
 *    W25Q64 shares the SPI bus (MISO/MOSI/SCK) with LoRa.
 *    Each device has its own CS pin. SPI.beginTransaction() / endTransaction()
 *    ensures clean bus handoff. We also explicitly deselect the LoRa CS pin
 *    before every flash operation.
 *
 *  No delay() calls. Uses millis()-bounded busy-wait for flash operations.
 * ============================================================================
 */

#ifndef FLASH_QUEUE_H
#define FLASH_QUEUE_H

#include <Arduino.h>
#include <SPI.h>

/* ── W25Q64 Constants ──────────────────────────────────────────────────── */
#define FLASH_TOTAL_SIZE     8388608UL   // 8 MB
#define FLASH_SECTOR_SIZE    4096U       // 4 KB per sector
#define FLASH_PAGE_SIZE      256U        // 256 bytes per page
#define FLASH_TOTAL_SECTORS  2048U       // 8MB / 4KB
#define FLASH_META_SECTOR    0           // Sector 0 = metadata log
#define FLASH_DATA_START     1           // Data begins at sector 1
#define FLASH_DATA_SECTORS   2045U       // Sectors available for data (1..2045)
#define FLASH_DATA_END       2046U       // Exclusive upper bound
#define FLASH_META_MAGIC     0xABCD
#define FLASH_META_ENTRY_SIZE 16         // Metadata entry size (aligned)

/* ── W25Q64 SPI Commands ───────────────────────────────────────────────── */
#define CMD_WRITE_ENABLE     0x06
#define CMD_WRITE_DISABLE    0x04
#define CMD_READ_STATUS1     0x05
#define CMD_READ_DATA        0x03
#define CMD_PAGE_PROGRAM     0x02
#define CMD_SECTOR_ERASE     0x20
#define CMD_CHIP_ERASE       0xC7
#define CMD_JEDEC_ID         0x9F
#define CMD_POWER_UP         0xAB

/* SPI settings for W25Q64 (8 MHz, compatible with ATmega328P @ 16 MHz) */
static const SPISettings flashSPISettings(8000000, MSBFIRST, SPI_MODE0);

/* ── Metadata Entry (16 bytes, page-aligned writes) ────────────────────── */
struct QueueMeta {
    uint16_t magic;         // FLASH_META_MAGIC = valid entry
    uint16_t writeSector;   // Current write sector (1..2045)
    uint16_t writeOffset;   // Byte offset within write sector
    uint16_t readSector;    // Current read sector (1..2045)
    uint16_t readOffset;    // Byte offset within read sector
    uint16_t count;         // Total entries in queue
    uint16_t metaPos;       // Next write position in sector 0
    uint16_t reserved;      // Pad to 16 bytes
} __attribute__((packed));


class FlashQueue {
public:
    uint8_t  csPin;
    uint8_t  loraCsPin;     // LoRa CS pin — ensured HIGH before flash ops
    uint16_t entrySize;
    uint16_t entriesPerSector;  // floor(4096 / entrySize)
    uint16_t maxEntries;        // entriesPerSector × FLASH_DATA_SECTORS

    /* Cached in RAM */
    uint16_t writeSector;
    uint16_t writeOffset;
    uint16_t readSector;
    uint16_t readOffset;
    uint16_t count;
    uint16_t metaPos;   // Next write offset in sector 0

    /* ── Initialize ────────────────────────────────────────────────────── */
    bool begin(uint8_t _csPin, uint16_t _entrySize, uint8_t _loraCsPin = 10) {
        csPin = _csPin;
        loraCsPin = _loraCsPin;
        entrySize = _entrySize;
        entriesPerSector = FLASH_SECTOR_SIZE / entrySize;
        maxEntries = (uint16_t)min((uint32_t)entriesPerSector * FLASH_DATA_SECTORS, (uint32_t)65535);

        pinMode(csPin, OUTPUT);
        digitalWrite(csPin, HIGH);

        /* Ensure LoRa CS is HIGH so it doesn't interfere with flash SPI */
        pinMode(loraCsPin, OUTPUT);
        digitalWrite(loraCsPin, HIGH);

        /* Wake up flash (in case it was in power-down) */
        _select();
        SPI.transfer(CMD_POWER_UP);
        _deselect();

        /* Verify JEDEC ID — W25Q64 = 0xEF4017 */
        uint32_t id = readJEDECID();
        if ((id >> 8) != 0xEF40) {
            /* Not a Winbond W25Q flash, but may still work */
            /* Accept any device that responds */
            if (id == 0 || id == 0xFFFFFF) {
                return false;   // No device found
            }
        }

        /* Scan sector 0 for last valid metadata entry */
        if (!_loadMetadata()) {
            format();   // First use or corrupted — initialize
        }

        return true;
    }

    /* ── Format — erase all and reset pointers ─────────────────────────── */
    void format() {
        /* Erase metadata sector */
        _sectorErase(0);

        writeSector = FLASH_DATA_START;
        writeOffset = 0;
        readSector  = FLASH_DATA_START;
        readOffset  = 0;
        count       = 0;
        metaPos     = 0;

        /* Erase the first data sector so it's ready for writes */
        _sectorErase((uint32_t)writeSector * FLASH_SECTOR_SIZE);

        _saveMeta();
    }

    /* ── Push entry onto queue tail ────────────────────────────────────── */
    bool push(const uint8_t* data) {
        if (count >= maxEntries) return false;

        /* Ensure LoRa CS is HIGH before flash operations */
        digitalWrite(loraCsPin, HIGH);

        /* Check if current sector has room */
        if (writeOffset + entrySize > FLASH_SECTOR_SIZE) {
            /* Advance to next data sector */
            writeSector = _nextDataSector(writeSector);
            writeOffset = 0;
            /* Erase the new sector before writing */
            _sectorErase((uint32_t)writeSector * FLASH_SECTOR_SIZE);
        }

        /* Write data at writeSector:writeOffset */
        uint32_t addr = (uint32_t)writeSector * FLASH_SECTOR_SIZE + writeOffset;
        _writeData(addr, data, entrySize);

        writeOffset += entrySize;
        count++;

        _saveMeta();
        return true;
    }

    /* ── Peek at queue head (read without removing) ────────────────────── */
    /*
     * IMPORTANT: peek() is READ-ONLY. It never modifies readSector or
     * readOffset. Sector advancement is done only by pop().
     */
    bool peek(uint8_t* data) {
        if (count == 0) return false;

        /* Ensure LoRa CS is HIGH before flash operations */
        digitalWrite(loraCsPin, HIGH);

        /* Calculate the actual read position, handling sector boundary */
        uint16_t peekSector = readSector;
        uint16_t peekOffset = readOffset;

        if (peekOffset + entrySize > FLASH_SECTOR_SIZE) {
            peekSector = _nextDataSector(peekSector);
            peekOffset = 0;
        }

        uint32_t addr = (uint32_t)peekSector * FLASH_SECTOR_SIZE + peekOffset;
        _readData(addr, data, entrySize);
        return true;
    }

    /* ── Pop head entry (advance read pointer) ─────────────────────────── */
    bool pop() {
        if (count == 0) return false;

        /* Ensure LoRa CS is HIGH before flash operations */
        digitalWrite(loraCsPin, HIGH);

        /* First, handle sector boundary (same logic as peek) */
        if (readOffset + entrySize > FLASH_SECTOR_SIZE) {
            readSector = _nextDataSector(readSector);
            readOffset = 0;
        }

        /* Now advance past the entry we're popping */
        readOffset += entrySize;
        count--;

        _saveMeta();
        return true;
    }

    uint16_t available() const { return count; }
    bool     isEmpty()   const { return count == 0; }
    bool     isFull()    const { return count >= maxEntries; }

    /* ── Read JEDEC ID (for verification) ──────────────────────────────── */
    uint32_t readJEDECID() {
        _select();
        SPI.transfer(CMD_JEDEC_ID);
        uint32_t id = (uint32_t)SPI.transfer(0) << 16;
        id |= (uint32_t)SPI.transfer(0) << 8;
        id |= SPI.transfer(0);
        _deselect();
        return id;
    }

    /* ── Persistent Sequence Counter (uses a reserved sector) ──────────── */
    /*
     * Saves the last-used sequence counter to a known flash sector so it
     * survives reboots. Uses append-log style with 4-byte entries.
     * When the sector fills up, it's erased and writing restarts.
     */
    void saveSequenceId(uint16_t seqSector, uint16_t seqId) {
        digitalWrite(loraCsPin, HIGH);

        /* Find next free 4-byte slot in the sector */
        uint32_t baseAddr = (uint32_t)seqSector * FLASH_SECTOR_SIZE;
        uint16_t pos = 0;
        uint8_t buf[4];

        /* Scan for first 0xFFFF (erased) magic */
        for (pos = 0; pos + 4 <= FLASH_SECTOR_SIZE; pos += 4) {
            _readData(baseAddr + pos, buf, 4);
            uint16_t magic = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            if (magic != 0xABCD) break; // Found erased or invalid slot
        }

        /* If sector is full, erase and restart */
        if (pos + 4 > FLASH_SECTOR_SIZE) {
            _sectorErase(baseAddr);
            pos = 0;
        }

        /* Write: [magic=0xABCD (2 bytes)] [seqId (2 bytes)] */
        buf[0] = 0xCD; buf[1] = 0xAB;  // Little-endian magic
        buf[2] = (uint8_t)(seqId & 0xFF);
        buf[3] = (uint8_t)(seqId >> 8);
        _writeData(baseAddr + pos, buf, 4);
    }

    uint16_t loadSequenceId(uint16_t seqSector) {
        digitalWrite(loraCsPin, HIGH);

        uint32_t baseAddr = (uint32_t)seqSector * FLASH_SECTOR_SIZE;
        uint16_t lastSeq = 0;
        bool found = false;
        uint8_t buf[4];

        /* Scan forward for the last valid entry */
        for (uint16_t pos = 0; pos + 4 <= FLASH_SECTOR_SIZE; pos += 4) {
            _readData(baseAddr + pos, buf, 4);
            uint16_t magic = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            if (magic == 0xABCD) {
                lastSeq = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
                found = true;
            } else {
                break;  // Reached erased area
            }
        }

        return found ? lastSeq : 0;
    }

private:
    /* ── SPI chip select with transaction ──────────────────────────────── */
    void _select() {
        SPI.beginTransaction(flashSPISettings);
        digitalWrite(csPin, LOW);
    }

    void _deselect() {
        digitalWrite(csPin, HIGH);
        SPI.endTransaction();
    }

    /* ── Write Enable (must precede every program/erase) ───────────────── */
    void _writeEnable() {
        _select();
        SPI.transfer(CMD_WRITE_ENABLE);
        _deselect();
    }

    /* ── Busy wait (polls status register, releases SPI between polls) ── */
    void _waitBusy() {
        unsigned long start = millis();
        while (millis() - start < 500) {    // 500 ms safety timeout
            _select();
            SPI.transfer(CMD_READ_STATUS1);
            uint8_t status = SPI.transfer(0);
            _deselect();
            if (!(status & 0x01)) return;   // BUSY bit clear = done
        }
    }

    /* ── Read Data (arbitrary length, no page boundary concern) ─────── */
    void _readData(uint32_t addr, uint8_t* buf, uint16_t len) {
        _select();
        SPI.transfer(CMD_READ_DATA);
        SPI.transfer((uint8_t)(addr >> 16));
        SPI.transfer((uint8_t)(addr >> 8));
        SPI.transfer((uint8_t)(addr));
        for (uint16_t i = 0; i < len; i++) {
            buf[i] = SPI.transfer(0);
        }
        _deselect();
    }

    /* ── Page Program (handles page boundary crossing) ─────────────────── */
    void _writeData(uint32_t addr, const uint8_t* buf, uint16_t len) {
        uint16_t offset = 0;
        while (offset < len) {
            /* Bytes remaining in current 256-byte page */
            uint16_t pageRemain = FLASH_PAGE_SIZE - (uint16_t)((addr + offset) % FLASH_PAGE_SIZE);
            uint16_t chunk = min(pageRemain, (uint16_t)(len - offset));

            _writeEnable();
            _select();
            SPI.transfer(CMD_PAGE_PROGRAM);
            uint32_t a = addr + offset;
            SPI.transfer((uint8_t)(a >> 16));
            SPI.transfer((uint8_t)(a >> 8));
            SPI.transfer((uint8_t)(a));
            for (uint16_t i = 0; i < chunk; i++) {
                SPI.transfer(buf[offset + i]);
            }
            _deselect();
            _waitBusy();    // Page program: ~0.7 ms typical, 3 ms max

            offset += chunk;
        }
    }

    /* ── Sector Erase (4 KB) ───────────────────────────────────────────── */
    void _sectorErase(uint32_t addr) {
        _writeEnable();
        _select();
        SPI.transfer(CMD_SECTOR_ERASE);
        SPI.transfer((uint8_t)(addr >> 16));
        SPI.transfer((uint8_t)(addr >> 8));
        SPI.transfer((uint8_t)(addr));
        _deselect();
        _waitBusy();    // Sector erase: ~45 ms typical, 400 ms max
    }

    /* ── Circular sector advance (within data sectors 1..2045) ─────────── */
    uint16_t _nextDataSector(uint16_t current) {
        current++;
        if (current >= FLASH_DATA_END) {
            current = FLASH_DATA_START;     // Wrap around
        }
        return current;
    }

    /* ── Save metadata to sector 0 (append-log style) ──────────────────── */
    void _saveMeta() {
        /* Check if sector 0 is full — erase and restart */
        if (metaPos + FLASH_META_ENTRY_SIZE > FLASH_SECTOR_SIZE) {
            _sectorErase(0);
            metaPos = 0;
        }

        QueueMeta meta;
        meta.magic       = FLASH_META_MAGIC;
        meta.writeSector = writeSector;
        meta.writeOffset = writeOffset;
        meta.readSector  = readSector;
        meta.readOffset  = readOffset;
        meta.count       = count;
        meta.metaPos     = metaPos + FLASH_META_ENTRY_SIZE; // Next position
        meta.reserved    = 0;

        _writeData((uint32_t)metaPos, (const uint8_t*)&meta, sizeof(QueueMeta));
        metaPos += FLASH_META_ENTRY_SIZE;
    }

    /* ── Load last valid metadata from sector 0 ────────────────────────── */
    bool _loadMetadata() {
        QueueMeta meta;
        QueueMeta lastValid;
        bool found = false;

        /*
         * Scan sector 0 forward. Each 16-byte entry is written sequentially.
         * The last entry with magic == 0xABCD is the most recent state.
         * Erased flash reads 0xFFFF (no valid magic).
         */
        for (uint16_t pos = 0; pos + FLASH_META_ENTRY_SIZE <= FLASH_SECTOR_SIZE;
             pos += FLASH_META_ENTRY_SIZE) {

            _readData((uint32_t)pos, (uint8_t*)&meta, sizeof(QueueMeta));

            if (meta.magic == FLASH_META_MAGIC) {
                lastValid = meta;
                found = true;
            } else {
                break;  // Reached unwritten area (0xFF)
            }
        }

        if (found) {
            writeSector = lastValid.writeSector;
            writeOffset = lastValid.writeOffset;
            readSector  = lastValid.readSector;
            readOffset  = lastValid.readOffset;
            count       = lastValid.count;
            metaPos     = lastValid.metaPos;

            /* Sanity check */
            if (writeSector == 0 || writeSector >= FLASH_DATA_END ||
                readSector == 0  || readSector >= FLASH_DATA_END ||
                metaPos > FLASH_SECTOR_SIZE) {
                return false;   // Corrupt — will trigger format()
            }
            return true;
        }

        return false;   // No valid metadata found
    }
};

#endif // FLASH_QUEUE_H
