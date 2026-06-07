-- ============================================================================
--  Weather Station Network — Database Schema
-- ============================================================================
--  MySQL 8.0+
--  Run: mysql -u root -p < schema.sql
-- ============================================================================

CREATE DATABASE IF NOT EXISTS weather_station
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE weather_station;

-- ── Main Readings Table ─────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS readings (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    node_id         TINYINT UNSIGNED NOT NULL COMMENT 'Node ID: 1-4=child, 5=parent',
    sequence_id     SMALLINT UNSIGNED NOT NULL COMMENT 'Packet sequence number',
    reading_time    DATETIME NOT NULL COMMENT 'Sensor reading timestamp from RTC',
    temp            FLOAT DEFAULT NULL COMMENT 'AHT10 temperature (°C)',
    humidity        FLOAT DEFAULT NULL COMMENT 'AHT10 relative humidity (%)',
    soil_temp_0     FLOAT DEFAULT NULL COMMENT 'Soil temp at 0m depth',
    soil_temp_20    FLOAT DEFAULT NULL COMMENT 'Soil temp at 20m depth',
    soil_temp_40    FLOAT DEFAULT NULL COMMENT 'Soil temp at 40m depth',
    soil_temp_60    FLOAT DEFAULT NULL COMMENT 'Soil temp at 60m depth',
    soil_temp_80    FLOAT DEFAULT NULL COMMENT 'Soil temp at 80m depth',
    leaf_wetness    SMALLINT UNSIGNED DEFAULT NULL COMMENT 'Leaf wetness ADC value',
    rain_gauge      SMALLINT UNSIGNED DEFAULT NULL COMMENT 'Tipping bucket count',
    security_alert  BOOLEAN DEFAULT FALSE COMMENT 'Security pin disconnected',
    sensor_status   TINYINT UNSIGNED DEFAULT 0 COMMENT 'Sensor health bitmask',
    received_at     DATETIME DEFAULT CURRENT_TIMESTAMP COMMENT 'Server receive time',

    -- Deduplication is handled application-side (15-minute received_at window)
    -- No UNIQUE KEY on (node_id, sequence_id) — the uint16_t counter wraps
    -- after ~455 days, which would block inserts for existing sequence IDs.
    INDEX idx_node_seq_time (node_id, sequence_id, reading_time),

    -- Query indexes
    INDEX idx_node_id (node_id),
    INDEX idx_reading_time (reading_time),
    INDEX idx_node_time (node_id, reading_time)
) ENGINE=InnoDB;

-- ── Node Registry (optional, for display names) ────────────────────────
CREATE TABLE IF NOT EXISTS nodes (
    node_id     TINYINT UNSIGNED PRIMARY KEY,
    name        VARCHAR(50) NOT NULL,
    type        ENUM('child', 'parent', 'main') NOT NULL,
    location    VARCHAR(100) DEFAULT NULL,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

INSERT IGNORE INTO nodes (node_id, name, type) VALUES
    (1, 'Child Station 01', 'child'),
    (2, 'Child Station 02', 'child'),
    (3, 'Child Station 03', 'child'),
    (4, 'Child Station 04', 'child'),
    (5, 'Parent Station',   'parent'),
    (6, 'Main Station',     'main');
