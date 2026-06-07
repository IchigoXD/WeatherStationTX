"""
============================================================================
 Weather Station Network — Flask Server
============================================================================
 Endpoints:
   POST /upload              — Receive binary DataPacket from Main Station
   GET  /                    — Dashboard UI
   GET  /api/readings        — Paginated readings (optional: node_id, limit, start, end)
   GET  /api/latest          — Latest reading per node
   GET  /api/averages        — Per-node averages (optional: node_id, hours)
   GET  /api/network_avg     — Network-wide averages
   GET  /api/time_series     — Time series for charts (field, node_id, hours)
   GET  /api/node_status     — Node online/offline status
   GET  /api/alive_check     — Queue health-check command
   POST /api/health_response — Receive health-check results from Main
   GET  /export              — Download all readings as CSV
============================================================================
"""

import struct
import io
import csv
import logging
import os
import math
from datetime import datetime

from flask import Flask, request, render_template, jsonify, Response
from functools import wraps
from dotenv import load_dotenv

from database_manager import DatabaseManager

# Load environment variables from .env file
load_dotenv()

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s'
)
logger = logging.getLogger('weather_server')

app = Flask(__name__)
db = DatabaseManager()

# ── DataPacket Binary Format (must match Protocol.h) ────────────────────
# struct DataPacket {
#   uint8_t  magic;          // 1
#   uint8_t  type;           // 1
#   uint8_t  sourceId;       // 1
#   uint8_t  reserved;       // 1
#   uint16_t sequenceId;     // 2
#   uint32_t timestamp;      // 4
#   struct SensorData {
#     float temp;            // 4
#     float humidity;        // 4
#     float soil_temp[5];    // 20
#     uint16_t leaf_wetness; // 2
#     uint16_t rain_gauge;   // 2
#     uint8_t security_alert;// 1
#     uint8_t sensor_status; // 1
#   };                       // = 34
#   int16_t  rssi;           // 2
#   float    snr;            // 4
#   uint16_t crc;            // 2
# };                         // Total = 52 (with __attribute__((packed)))

PACKET_FORMAT = '<BBBBHIff5fHHBBhfH'
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)
MAGIC_BYTE = 0xAF

# Command queue for health checks
pending_commands = []
health_results = {}

# AP-04: Optional API key for health endpoints (set API_KEY in .env to enable)
API_KEY = os.getenv('API_KEY', '')

def require_api_key(f):
    """Decorator: enforce X-API-KEY header if API_KEY is configured."""
    @wraps(f)
    def decorated(*args, **kwargs):
        if API_KEY:
            provided = request.headers.get('X-API-KEY', '')
            if provided != API_KEY:
                return jsonify({'error': 'Unauthorized — invalid or missing API key'}), 403
        return f(*args, **kwargs)
    return decorated

def calculate_dew_point(temp, humidity):
    """Calculate dew point using Magnus-Tetens formula (approx ±0.4°C)."""
    if temp is None or humidity is None or humidity == 0:
        return None
    # Magnus formula constants
    a = 17.27
    b = 237.7
    try:
        alpha = ((a * temp) / (b + temp)) + math.log(humidity / 100.0)
        dp = (b * alpha) / (a - alpha)
        return round(dp, 2)
    except (ValueError, ZeroDivisionError, OverflowError):
        return None

def _process_reading(r):
    """Add calculated fields and format standard response."""
    if r:
        r['dew_point'] = calculate_dew_point(r.get('temp'), r.get('humidity'))
    return r


# ── Upload Endpoint ─────────────────────────────────────────────────────

@app.route('/upload', methods=['POST'])
def upload():
    """Receive binary DataPacket from Main Station via SIM800A."""
    data = request.get_data()

    if len(data) < PACKET_SIZE:
        logger.warning(f"Received undersized packet: {len(data)} bytes (expected {PACKET_SIZE})")
        return "Data too short", 400

    try:
        unpacked = struct.unpack(PACKET_FORMAT, data[:PACKET_SIZE])

        magic       = unpacked[0]
        pkt_type    = unpacked[1]
        source_id   = unpacked[2]
        # reserved  = unpacked[3]
        sequence_id = unpacked[4]
        timestamp   = unpacked[5]
        temp        = unpacked[6]
        humidity    = unpacked[7]
        soil_temps  = unpacked[8:13]
        leaf_wet    = unpacked[13]
        rain_gauge  = unpacked[14]
        sec_alert   = unpacked[15]
        sen_status  = unpacked[16]
        rssi        = unpacked[17]
        snr         = unpacked[18]
        crc         = unpacked[19]

        # Validate magic byte
        if magic != MAGIC_BYTE:
            logger.warning(f"Invalid magic byte: 0x{magic:02X}")
            return "Invalid magic byte", 400

        # CRC validation (CRC16-CCITT over all bytes except last 2)
        crc_data = data[:PACKET_SIZE - 2]
        computed_crc = _crc16_ccitt(crc_data)
        if computed_crc != crc:
            logger.warning(f"CRC mismatch: computed=0x{computed_crc:04X} received=0x{crc:04X}")
            return "CRC mismatch", 400

        packet = {
            'node_id':        source_id,
            'sequence_id':    sequence_id,
            'timestamp':      timestamp,
            'temp':           round(temp, 2),
            'humidity':       round(humidity, 2),
            'soil_temp':      [round(s, 2) for s in soil_temps],
            'leaf_wetness':   leaf_wet,       # Raw ADC count — no rounding
            'rain_gauge':     rain_gauge,     # Raw tip count — no rounding
            'security_alert': bool(sec_alert),
            'sensor_status':  sen_status,
            'rssi':           rssi,
            'snr':            round(snr, 1),
        }

        db.insert_reading(packet)
        db.update_node_seen(6)          # Main Station (Node 6) is online (it sent this request)
        db.update_node_seen(source_id)  # The originating node is also online

        logger.info(
            f"Received: node={source_id} seq={sequence_id} "
            f"temp={temp:.1f}°C hum={humidity:.1f}%"
        )

        # If there's a pending command, include it in the response
        if pending_commands:
            cmd = pending_commands.pop(0)
            return cmd, 200

        return "OK", 200

    except struct.error as e:
        logger.error(f"Struct unpack error: {e}")
        return f"Parse error: {e}", 400
    except Exception as e:
        logger.error(f"Upload error: {e}", exc_info=True)
        return f"Server error: {e}", 500


# ── CRC16-CCITT (matches the firmware implementation) ───────────────────

def _crc16_ccitt(data: bytes) -> int:
    """CRC16-CCITT with 0xFFFF initial value, matching Protocol.h."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc


# ── Dashboard ───────────────────────────────────────────────────────────

@app.route('/')
def index():
    """Serve the main dashboard."""
    return render_template('dashboard.html')


# ── API: Readings ───────────────────────────────────────────────────────

@app.route('/api/readings')
def api_readings():
    """Get paginated readings with optional filters."""
    node_id    = request.args.get('node_id', type=int)
    limit      = request.args.get('limit', 100, type=int)
    offset     = request.args.get('offset', 0, type=int)
    start_date = request.args.get('start')
    end_date   = request.args.get('end')

    readings = db.get_readings(
        node_id=node_id,
        limit=min(limit, 1000),
        offset=offset,
        start_date=start_date,
        end_date=end_date
    )
    for r in readings:
        _process_reading(r)
    return jsonify(readings)


# ── API: Latest Per Node ────────────────────────────────────────────────

@app.route('/api/latest')
def api_latest():
    """Get the most recent reading from each node."""
    data = db.get_latest_per_node()
    for r in data:
        _process_reading(r)
    return jsonify(data)


# ── API: Averages ───────────────────────────────────────────────────────

@app.route('/api/averages')
def api_averages():
    """Get per-node average readings over a time period."""
    node_id = request.args.get('node_id', type=int)
    hours   = request.args.get('hours', 24, type=int)
    data = db.get_averages(node_id=node_id, hours=hours)
    for r in data:
        # Calculate dew point based on averaged temp/humidity
        r['avg_dew_point'] = calculate_dew_point(r.get('avg_temp'), r.get('avg_humidity'))
    return jsonify(data)


# ── API: Network Averages ──────────────────────────────────────────────

@app.route('/api/network_avg')
def api_network_avg():
    """Get network-wide combined averages."""
    hours = request.args.get('hours', 24, type=int)
    return jsonify(db.get_network_averages(hours=hours))


# ── API: Time Series ───────────────────────────────────────────────────

@app.route('/api/time_series')
def api_time_series():
    """Get time-series data for a specific field for charting."""
    node_id = request.args.get('node_id', type=int)
    hours   = request.args.get('hours', 24, type=int)
    field   = request.args.get('field', 'temp')
    
    # Whitelist allowed fields to prevent SQL injection
    # 'dew_point' is special as it requires multiple DB fields
    allowed_db_fields = {
        'temp', 'humidity', 'soil_temp_0', 'soil_temp_20',
        'soil_temp_40', 'soil_temp_60', 'soil_temp_80',
        'leaf_wetness', 'rain_gauge'
    }

    if field == 'dew_point':
        # Need both temp and humidity to compute dew point
        data = db.get_time_series_multi(node_id=node_id, hours=hours, fields=['temp', 'humidity'])
        for r in data:
            r['dew_point'] = calculate_dew_point(r.get('temp'), r.get('humidity'))
            # Clean up raw fields if requested ONLY for dew_point
            del r['temp']
            del r['humidity']
        return jsonify(data)

    if field not in allowed_db_fields:
        return jsonify({'error': f'Invalid field. Allowed: {list(allowed_db_fields) + ["dew_point"]}'}), 400

    return jsonify(db.get_time_series(node_id=node_id, hours=hours, field=field))


# ── API: Node Status ───────────────────────────────────────────────────

@app.route('/api/node_status')
def api_node_status():
    """Get online/offline status for each registered node."""
    return jsonify(db.get_node_status())


# ── API: Health Check ──────────────────────────────────────────────────

@app.route('/api/alive_check')
@require_api_key
def trigger_alive_check():
    """Queue a CHECK_ALIVE command for the Main Station."""
    pending_commands.append("CHECK_ALIVE")
    logger.info("Health check command queued for Main Station")
    return jsonify({
        'status': 'queued',
        'message': 'CHECK_ALIVE command queued. Will be sent on next Main Station upload.'
    })


@app.route('/api/health_response')
@require_api_key
def receive_health_response():
    """Receive health-check flags from Main Station (via GET with ?flags=XX)."""
    flags = request.args.get('flags', 0, type=int)

    result = {
        'child_1': bool(flags & 0x01),
        'child_2': bool(flags & 0x02),
        'child_3': bool(flags & 0x04),
        'child_4': bool(flags & 0x08),
        'parent':  bool(flags & 0x10),
        'raw_flags': flags,
        'timestamp': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    }

    health_results['latest'] = result
    
    # Update last_seen in DB for nodes that responded
    db.update_node_seen(6) # Main station sent this
    if result['child_1']: db.update_node_seen(1)
    if result['child_2']: db.update_node_seen(2)
    if result['child_3']: db.update_node_seen(3)
    if result['child_4']: db.update_node_seen(4)
    if result['parent']: db.update_node_seen(5)

    logger.info(f"Health check result: flags=0x{flags:02X} → {result}")
    return "OK", 200


@app.route('/api/health_status')
def get_health_status():
    """Get the latest health-check result (not saved to DB per spec)."""
    if 'latest' in health_results:
        return jsonify(health_results['latest'])
    return jsonify({'status': 'no_check_performed'}), 404


# ── CSV Export ──────────────────────────────────────────────────────────

@app.route('/export')
def export_csv():
    """Export all readings as a CSV file download."""
    node_id    = request.args.get('node_id', type=int)
    start_date = request.args.get('start')
    end_date   = request.args.get('end')

    readings = db.get_readings(
        node_id=node_id, limit=100000,
        start_date=start_date, end_date=end_date
    )

    def generate():
        output = io.StringIO()
        writer = csv.writer(output)

        # Header row
        writer.writerow([
            'ID', 'Node ID', 'Node Name', 'Sequence', 'Reading Time',
            'Temperature (°C)', 'Humidity (%)',
            'Soil 0m (°C)', 'Soil 20m (°C)', 'Soil 40m (°C)',
            'Soil 60m (°C)', 'Soil 80m (°C)',
            'Leaf Wetness', 'Rain Gauge', 'Security Alert',
            'Sensor Status', 'Received At'
        ])
        yield output.getvalue()
        output.seek(0)
        output.truncate(0)

        # Data rows
        for r in readings:
            writer.writerow([
                r.get('id'), r.get('node_id'), r.get('node_name', ''),
                r.get('sequence_id'), r.get('reading_time'),
                r.get('temp'), r.get('humidity'),
                r.get('soil_temp_0'), r.get('soil_temp_20'),
                r.get('soil_temp_40'), r.get('soil_temp_60'),
                r.get('soil_temp_80'),
                r.get('leaf_wetness'), r.get('rain_gauge'),
                r.get('security_alert'), r.get('sensor_status'),
                r.get('received_at')
            ])
            yield output.getvalue()
            output.seek(0)
            output.truncate(0)

    filename = f"weather_data_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
    response = Response(generate(), mimetype='text/csv')
    response.headers.set("Content-Disposition", "attachment", filename=filename)
    return response


# ── Main ────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    host = os.getenv('FLASK_HOST', '0.0.0.0')
    port = int(os.getenv('FLASK_PORT', '5000'))
    debug = os.getenv('FLASK_DEBUG', 'true').lower() == 'true'

    logger.info(f"Starting Weather Station Server on {host}:{port}")
    app.run(host=host, port=port, debug=debug)
