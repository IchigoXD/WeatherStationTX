"""
============================================================================
 Weather Station Network — Database Manager
============================================================================
 MySQL database interface with connection pooling, environment-based
 configuration, and query methods for readings, averages, and time ranges.
============================================================================
"""

import os
import logging
from datetime import datetime, timedelta
from contextlib import contextmanager

import mysql.connector
from mysql.connector import pooling, errors

logger = logging.getLogger(__name__)


class DatabaseManager:
    """Thread-safe MySQL database manager with connection pooling."""

    def __init__(self):
        self.config = {
            'host':     os.getenv('DB_HOST', 'localhost'),
            'port':     int(os.getenv('DB_PORT', '3306')),
            'user':     os.getenv('DB_USER', 'root'),
            'password': os.getenv('DB_PASSWORD', ''),
            'database': os.getenv('DB_NAME', 'weather_station'),
        }
        self._ensure_database()
        self.pool = pooling.MySQLConnectionPool(
            pool_name="weather_pool",
            pool_size=32,
            pool_reset_session=True,
            **self.config
        )
        self._ensure_tables()
        logger.info("Database connection pool initialized (size=32)")

    def _ensure_database(self):
        """Create the database if it doesn't exist."""
        cfg = {k: v for k, v in self.config.items() if k != 'database'}
        conn = mysql.connector.connect(**cfg)
        cursor = conn.cursor()
        cursor.execute(
            f"CREATE DATABASE IF NOT EXISTS `{self.config['database']}` "
            f"CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
        )
        conn.commit()
        cursor.close()
        conn.close()

    def _ensure_tables(self):
        """Create tables if they don't exist."""
        with self.get_connection() as (conn, cursor):
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS readings (
                    id              INT AUTO_INCREMENT PRIMARY KEY,
                    node_id         TINYINT UNSIGNED NOT NULL,
                    sequence_id     SMALLINT UNSIGNED NOT NULL,
                    reading_time    DATETIME NOT NULL,
                    temp            FLOAT DEFAULT NULL,
                    humidity        FLOAT DEFAULT NULL,
                    soil_temp_0     FLOAT DEFAULT NULL,
                    soil_temp_20    FLOAT DEFAULT NULL,
                    soil_temp_40    FLOAT DEFAULT NULL,
                    soil_temp_60    FLOAT DEFAULT NULL,
                    soil_temp_80    FLOAT DEFAULT NULL,
                    leaf_wetness    SMALLINT UNSIGNED DEFAULT NULL,
                    rain_gauge      SMALLINT UNSIGNED DEFAULT NULL,
                    security_alert  BOOLEAN DEFAULT FALSE,
                    sensor_status   TINYINT UNSIGNED DEFAULT 0,
                    rssi            SMALLINT DEFAULT NULL,
                    snr             FLOAT DEFAULT NULL,
                    received_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
                    INDEX idx_node_seq_time (node_id, sequence_id, reading_time),
                    INDEX idx_node_id (node_id),
                    INDEX idx_reading_time (reading_time),
                    INDEX idx_node_time (node_id, reading_time)
                ) ENGINE=InnoDB
            """)
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS nodes (
                    node_id     TINYINT UNSIGNED PRIMARY KEY,
                    name        VARCHAR(50) NOT NULL,
                    type        ENUM('child', 'parent', 'main') NOT NULL,
                    location    VARCHAR(100) DEFAULT NULL,
                    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
                    last_seen   DATETIME DEFAULT NULL
                ) ENGINE=InnoDB
            """)
            # Insert default nodes
            cursor.execute("""
                INSERT IGNORE INTO nodes (node_id, name, type) VALUES
                    (1, 'Child Station 01', 'child'),
                    (2, 'Child Station 02', 'child'),
                    (3, 'Child Station 03', 'child'),
                    (4, 'Child Station 04', 'child'),
                    (5, 'Parent Station',   'parent'),
                    (6, 'Main Station',     'main')
            """)
            
            # Schema migration: Add rssi/snr if they don't exist
            cursor.execute("SHOW COLUMNS FROM readings LIKE 'rssi'")
            if not cursor.fetchone():
                logger.info("Migrating database: Adding 'rssi' column to 'readings'")
                cursor.execute("ALTER TABLE readings ADD COLUMN rssi SMALLINT DEFAULT NULL AFTER sensor_status")
            
            cursor.execute("SHOW COLUMNS FROM readings LIKE 'snr'")
            if not cursor.fetchone():
                logger.info("Migrating database: Adding 'snr' column to 'readings'")
                cursor.execute("ALTER TABLE readings ADD COLUMN snr FLOAT DEFAULT NULL AFTER rssi")

            cursor.execute("SHOW COLUMNS FROM nodes LIKE 'last_seen'")
            if not cursor.fetchone():
                logger.info("Migrating database: Adding 'last_seen' column to 'nodes'")
                cursor.execute("ALTER TABLE nodes ADD COLUMN last_seen DATETIME DEFAULT NULL AFTER created_at")

            conn.commit()

    @contextmanager
    def get_connection(self):
        """Context manager for pooled database connections."""
        conn = self.pool.get_connection()
        try:
            cursor = conn.cursor(dictionary=True)
            try:
                yield conn, cursor
            finally:
                cursor.close()
        finally:
            conn.close()

    # ── INSERT ──────────────────────────────────────────────────────────

    def insert_reading(self, data):
        """Insert a sensor reading with 15-minute deduplication for GPRS retries."""
        # Treat timestamp as invalid if it's before Jan 1, 2020 (1577836800)
        # This handles cases where a node's RTC has a dead battery and defaults to year 2000.
        pkt_time = data.get('timestamp', 0)
        if pkt_time < 1577836800:
            reading_time = datetime.now()
        else:
            reading_time = datetime.fromtimestamp(pkt_time)

        # Deduplication check for retries.
        # DB-03: Use received_at (server clock) instead of reading_time (RTC) to
        # prevent drift-related false positives/negatives.
        check_query = """
            SELECT id FROM readings
            WHERE node_id = %s AND sequence_id = %s AND received_at >= NOW() - INTERVAL 15 MINUTE
            LIMIT 1
        """
        
        insert_query = """
            INSERT INTO readings
            (node_id, sequence_id, reading_time, temp, humidity,
             soil_temp_0, soil_temp_20, soil_temp_40, soil_temp_60, soil_temp_80,
             leaf_wetness, rain_gauge, security_alert, sensor_status, rssi, snr)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        """

        values = (
            data['node_id'],
            data.get('sequence_id', 0),
            reading_time,
            data['temp'],
            data['humidity'],
            data['soil_temp'][0], data['soil_temp'][1], data['soil_temp'][2],
            data['soil_temp'][3], data['soil_temp'][4],
            data['leaf_wetness'],
            data['rain_gauge'],
            data.get('security_alert', False),
            data.get('sensor_status', 0),
            data.get('rssi'),
            data.get('snr'),
        )

        with self.get_connection() as (conn, cursor):
            # Check for recent duplicate
            cursor.execute(check_query, (data['node_id'], data.get('sequence_id', 0)))
            if cursor.fetchone():
                logger.warning(f"Recent duplicate ignored: node={data['node_id']} seq={data.get('sequence_id')}")
                return False

            try:
                cursor.execute(insert_query, values)
                conn.commit()
                inserted = cursor.rowcount > 0
            except errors.IntegrityError:
                # DB-02: Race condition — another thread/process inserted the same
                # (node_id, sequence_id) between our SELECT and INSERT.
                # Treat as a duplicate and move on.
                conn.rollback()
                logger.warning(f"IntegrityError duplicate: node={data['node_id']} seq={data.get('sequence_id')}")
                return False

        if inserted:
            logger.info(f"Inserted reading: node={data['node_id']} seq={data.get('sequence_id')}")

        return inserted

    def update_node_seen(self, node_id):
        """Update the last_seen timestamp for a specific node."""
        with self.get_connection() as (conn, cursor):
            cursor.execute("UPDATE nodes SET last_seen = NOW() WHERE node_id = %s", (node_id,))
            conn.commit()

    # ── QUERY: All Readings ─────────────────────────────────────────────

    def get_readings(self, node_id=None, limit=100, offset=0,
                     start_date=None, end_date=None):
        """Get readings with optional filters."""
        conditions = []
        params = []

        if node_id is not None:
            conditions.append("r.node_id = %s")
            params.append(int(node_id))
        if start_date:
            conditions.append("r.reading_time >= %s")
            params.append(start_date)
        if end_date:
            conditions.append("r.reading_time <= %s")
            params.append(end_date)

        where = "WHERE " + " AND ".join(conditions) if conditions else ""

        query = f"""
            SELECT r.*, n.name as node_name
            FROM readings r
            LEFT JOIN nodes n ON r.node_id = n.node_id
            {where}
            ORDER BY r.reading_time DESC
            LIMIT %s OFFSET %s
        """
        params.extend([limit, offset])

        with self.get_connection() as (conn, cursor):
            cursor.execute(query, params)
            results = cursor.fetchall()

        # Convert datetime objects to strings for JSON serialization
        for r in results:
            if r.get('reading_time'):
                r['reading_time'] = r['reading_time'].strftime('%Y-%m-%d %H:%M:%S')
            if r.get('received_at'):
                r['received_at'] = r['received_at'].strftime('%Y-%m-%d %H:%M:%S')
            if r.get('created_at'):
                r['created_at'] = r['created_at'].strftime('%Y-%m-%d %H:%M:%S')

        return results

    # ── QUERY: Latest Reading Per Node ──────────────────────────────────

    def get_latest_per_node(self):
        """Get the most recent reading from each node."""
        query = """
            SELECT r.*, n.name as node_name
            FROM readings r
            INNER JOIN (
                SELECT node_id, MAX(reading_time) as max_time
                FROM readings
                GROUP BY node_id
            ) latest ON r.node_id = latest.node_id AND r.reading_time = latest.max_time
            LEFT JOIN nodes n ON r.node_id = n.node_id
            ORDER BY r.node_id
        """
        with self.get_connection() as (conn, cursor):
            cursor.execute(query)
            results = cursor.fetchall()

        for r in results:
            if r.get('reading_time'):
                r['reading_time'] = r['reading_time'].strftime('%Y-%m-%d %H:%M:%S')
            if r.get('received_at'):
                r['received_at'] = r['received_at'].strftime('%Y-%m-%d %H:%M:%S')

        return results

    # ── QUERY: Averages ─────────────────────────────────────────────────

    def get_averages(self, node_id=None, hours=24):
        """Get average readings over a time period, optionally per node."""
        since = datetime.now() - timedelta(hours=hours)
        conditions = ["reading_time >= %s"]
        params = [since]

        group_by = "node_id"
        if node_id is not None:
            conditions.append("node_id = %s")
            params.append(int(node_id))

        where = "WHERE " + " AND ".join(conditions)

        query = f"""
            SELECT
                node_id,
                COUNT(*) as reading_count,
                ROUND(AVG(temp), 2) as avg_temp,
                ROUND(AVG(humidity), 2) as avg_humidity,
                ROUND(AVG(soil_temp_0), 2) as avg_soil_0,
                ROUND(AVG(soil_temp_20), 2) as avg_soil_20,
                ROUND(AVG(soil_temp_40), 2) as avg_soil_40,
                ROUND(AVG(soil_temp_60), 2) as avg_soil_60,
                ROUND(AVG(soil_temp_80), 2) as avg_soil_80,
                ROUND(AVG(leaf_wetness), 0) as avg_leaf_wetness,
                SUM(rain_gauge) as total_rain,
                MIN(temp) as min_temp,
                MAX(temp) as max_temp,
                MIN(humidity) as min_humidity,
                MAX(humidity) as max_humidity
            FROM readings
            {where}
            GROUP BY {group_by}
            ORDER BY node_id
        """
        with self.get_connection() as (conn, cursor):
            cursor.execute(query, params)
            results = cursor.fetchall()

        # Convert Decimal types to float for JSON
        for r in results:
            for key in r:
                if hasattr(r[key], '__float__'):
                    r[key] = float(r[key])

        return results

    # ── QUERY: Network Averages (all nodes combined) ────────────────────

    def get_network_averages(self, hours=24):
        """Get network-wide averages (all nodes combined)."""
        since = datetime.now() - timedelta(hours=hours)

        query = """
            SELECT
                COUNT(*) as total_readings,
                COUNT(DISTINCT node_id) as active_nodes,
                ROUND(AVG(temp), 2) as avg_temp,
                ROUND(AVG(humidity), 2) as avg_humidity,
                ROUND(AVG(soil_temp_0), 2) as avg_soil_0,
                ROUND(AVG(soil_temp_20), 2) as avg_soil_20,
                ROUND(AVG(soil_temp_40), 2) as avg_soil_40,
                ROUND(AVG(soil_temp_60), 2) as avg_soil_60,
                ROUND(AVG(soil_temp_80), 2) as avg_soil_80,
                ROUND(AVG(leaf_wetness), 0) as avg_leaf_wetness,
                SUM(rain_gauge) as total_rain
            FROM readings
            WHERE reading_time >= %s
        """
        with self.get_connection() as (conn, cursor):
            cursor.execute(query, (since,))
            result = cursor.fetchone()

        if result:
            for key in result:
                if hasattr(result[key], '__float__'):
                    result[key] = float(result[key])

        return result

    # ── QUERY: Time Series for Charts ───────────────────────────────────

    def get_time_series(self, node_id=None, hours=24, field='temp'):
        """Get time-series data for a specific field, suitable for charting."""
        # Whitelist validation to prevent SQL injection
        allowed_fields = {
            'temp', 'humidity', 'soil_temp_0', 'soil_temp_20',
            'soil_temp_40', 'soil_temp_60', 'soil_temp_80',
            'leaf_wetness', 'rain_gauge'
        }
        if field not in allowed_fields:
            logger.warning(f"Rejected invalid field: {field}")
            return []

        since = datetime.now() - timedelta(hours=hours)
        conditions = ["reading_time >= %s"]
        params = [since]

        if node_id is not None:
            conditions.append("node_id = %s")
            params.append(int(node_id))

        where = "WHERE " + " AND ".join(conditions)

        query = f"""
            SELECT node_id, reading_time, {field}
            FROM readings
            {where}
            ORDER BY reading_time ASC
        """
        with self.get_connection() as (conn, cursor):
            cursor.execute(query, params)
            results = cursor.fetchall()

        for r in results:
            if r.get('reading_time'):
                r['reading_time'] = r['reading_time'].strftime('%Y-%m-%d %H:%M:%S')

        return results

    def get_time_series_multi(self, node_id=None, hours=24, fields=None):
        """Get time-series data for multiple fields at once."""
        if not fields:
            return []

        # Whitelist validation to prevent SQL injection
        allowed_fields = {
            'temp', 'humidity', 'soil_temp_0', 'soil_temp_20',
            'soil_temp_40', 'soil_temp_60', 'soil_temp_80',
            'leaf_wetness', 'rain_gauge'
        }
        safe_fields = [f for f in fields if f in allowed_fields]
        if not safe_fields:
            logger.warning(f"All fields rejected as invalid: {fields}")
            return []

        since = datetime.now() - timedelta(hours=hours)
        conditions = ["reading_time >= %s"]
        params = [since]

        if node_id is not None:
            conditions.append("node_id = %s")
            params.append(int(node_id))

        where = "WHERE " + " AND ".join(conditions)
        field_list = ", ".join(safe_fields)

        query = f"""
            SELECT node_id, reading_time, {field_list}
            FROM readings
            {where}
            ORDER BY reading_time ASC
        """
        with self.get_connection() as (conn, cursor):
            cursor.execute(query, params)
            results = cursor.fetchall()

        for r in results:
            if r.get('reading_time'):
                r['reading_time'] = r['reading_time'].strftime('%Y-%m-%d %H:%M:%S')

        return results

    # ── QUERY: Node Status ──────────────────────────────────────────────

    def get_node_status(self):
        """Get last-seen time and status for each registered node."""
        query = """
            SELECT
                n.node_id,
                n.name,
                n.type,
                COALESCE(n.last_seen, r.reading_time) as last_seen,
                r.security_alert,
                r.sensor_status,
                TIMESTAMPDIFF(MINUTE, COALESCE(n.last_seen, r.reading_time), NOW()) as minutes_ago
            FROM nodes n
            LEFT JOIN (
                SELECT node_id, MAX(reading_time) as max_time
                FROM readings GROUP BY node_id
            ) latest ON n.node_id = latest.node_id
            LEFT JOIN readings r ON r.node_id = latest.node_id
                AND r.reading_time = latest.max_time
            ORDER BY n.node_id
        """
        with self.get_connection() as (conn, cursor):
            cursor.execute(query)
            results = cursor.fetchall()

        for r in results:
            if r.get('last_seen'):
                r['last_seen'] = r['last_seen'].strftime('%Y-%m-%d %H:%M:%S')
            # Consider node offline if no data in last 30 minutes
            r['online'] = (r.get('minutes_ago') or 999) < 30

        return results
