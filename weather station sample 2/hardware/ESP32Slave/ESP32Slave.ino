/*
 * ============================================================================
 *  ESP32 Sensor Slave — Child Station Variant
 * ============================================================================
 *  Reads: 5× DS18B20 soil temperature probes, capacitive leaf wetness sensor
 *  Communicates: I2C slave to ATmega328P master
 *
 *  Non-blocking design:
 *    - Sensors sampled every 2 seconds into a cached struct
 *    - I2C onRequest ISR sends cached data instantly
 *    - Watchdog timer resets on hang
 *    - Small millis()-based delay between sensor reads to avoid bus contention
 *
 *  If a sensor is disconnected, its value is set to 0 (per spec).
 * ============================================================================
 */

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>

/* ── Pin Definitions ───────────────────────────────────────────────────── */
#define ONE_WIRE_BUS    4           // DS18B20 data line
#define LEAF_SENSOR_PIN 34          // Capacitive leaf wetness (analog)
#define I2C_SDA         21          // Default ESP32 I2C SDA
#define I2C_SCL         22          // Default ESP32 I2C SCL
#define SLAVE_ADDR      0x08        // Must match I2C_ESP32_SLAVE in Config.h

/* ── Sensor Data Structure (MUST match SlavePayloadChild in Protocol.h) ── */
/*
 * If Protocol.h changes SlavePayloadChild, update this struct to match.
 * The static_assert below will catch any size mismatch at compile time.
 */
struct SlavePayload {
    float    soil_temp[5];          // 5 depth probes (20 bytes)
    uint16_t leaf_wetness;          // ADC value (2 bytes)
    uint8_t  sensor_status;         // Bitmask: bit1=DS18B20 ok, bit2=leaf ok
    uint8_t  padding;               // Alignment
} __attribute__((packed));          // Total: 24 bytes
static_assert(sizeof(SlavePayload) == 24, "SlavePayload must be 24 bytes (match SlavePayloadChild in Protocol.h)");

/* ── Globals ───────────────────────────────────────────────────────────── */
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

volatile SlavePayload cachedPayload;    // Accessed from ISR — must be volatile
portMUX_TYPE payloadMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool dataReady = false;
unsigned long lastSampleTime = 0;
const unsigned long SAMPLE_INTERVAL = 2000;  // 2 seconds

uint8_t sensorCount = 0;                // Number of DS18B20 found on bus

/* ── Sub-state for non-blocking sensor reads ───────────────────────────── */
enum SampleState : uint8_t {
    SAMPLE_IDLE,
    SAMPLE_DS18B20_REQUEST,     // Issue requestTemperatures()
    SAMPLE_WAIT_DS18B20,        // Wait for conversion (750ms @ 12-bit)
    SAMPLE_READ_DS18B20,        // Read results
    SAMPLE_WAIT_LEAF,           // Small gap before leaf read
    SAMPLE_READ_LEAF,           // Read leaf sensor
    SAMPLE_DONE                 // Copy to cached payload
};

SampleState sampleState = SAMPLE_IDLE;
unsigned long sampleStepTime = 0;
SlavePayload buildPayload;
uint8_t buildStatus = 0;

/* DS18B20 conversion time: ~750ms at 12-bit resolution */
const unsigned long DS18B20_CONV_MS = 800;
/* Inter-sensor delay */
const unsigned long INTER_SENSOR_DELAY_MS = 50;

/* ── Forward Declarations ──────────────────────────────────────────────── */
void onI2CRequest();

/* ── Setup ─────────────────────────────────────────────────────────────── */
void setup() {
    Serial.begin(115200);
    Serial.println(F("[ESP32-Child] Sensor Slave Initializing..."));

    /* DS18B20 initialization */
    ds18b20.begin();
    sensorCount = ds18b20.getDeviceCount();
    Serial.print(F("[ESP32-Child] DS18B20 sensors found: "));
    Serial.println(sensorCount);

    /* Set async mode for non-blocking temperature reads */
    ds18b20.setWaitForConversion(false);

    /* Initialize payload to zeros */
    memset((void*)&cachedPayload, 0, sizeof(SlavePayload));
    memset(&buildPayload, 0, sizeof(SlavePayload));

    /* I2C Slave setup */
    Wire.begin(SLAVE_ADDR);
    Wire.onRequest(onI2CRequest);

    /* Watchdog — reset if loop hangs for >10 seconds */
#if defined(ESP_IDF_VERSION) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 10000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&twdt_config);
    esp_task_wdt_add(NULL);
#else
    esp_task_wdt_init(10, true);
    esp_task_wdt_add(NULL);
#endif

    Serial.println(F("[ESP32-Child] Ready."));
}

/* ── Main Loop (non-blocking) ──────────────────────────────────────────── */
void loop() {
    esp_task_wdt_reset();

    unsigned long now = millis();

    /* Trigger new sample cycle every SAMPLE_INTERVAL */
    if (sampleState == SAMPLE_IDLE && now - lastSampleTime >= SAMPLE_INTERVAL) {
        lastSampleTime = now;
        sampleState = SAMPLE_DS18B20_REQUEST;
        sampleStepTime = now;
        memset(&buildPayload, 0, sizeof(SlavePayload));
        buildStatus = 0;
    }

    /* Non-blocking sensor read state machine */
    switch (sampleState) {
        case SAMPLE_IDLE:
            break;

        case SAMPLE_DS18B20_REQUEST:
            ds18b20.requestTemperatures();
            sampleState = SAMPLE_WAIT_DS18B20;
            sampleStepTime = now;
            break;

        case SAMPLE_WAIT_DS18B20:
            if (now - sampleStepTime >= DS18B20_CONV_MS) {
                sampleState = SAMPLE_READ_DS18B20;
            }
            break;

        case SAMPLE_READ_DS18B20: {
            bool anyDS18B20OK = false;
            for (uint8_t i = 0; i < 5; i++) {
                if (i < sensorCount) {
                    float t = ds18b20.getTempCByIndex(i);
                    if (t == DEVICE_DISCONNECTED_C || isnan(t) || t < -55.0f || t > 125.0f) {
                        buildPayload.soil_temp[i] = 0.0f;
                    } else {
                        buildPayload.soil_temp[i] = t;
                        anyDS18B20OK = true;
                    }
                } else {
                    buildPayload.soil_temp[i] = 0.0f;
                }
            }
            if (anyDS18B20OK) buildStatus |= 0x02;  // bit1 = DS18B20 OK

            sampleState = SAMPLE_WAIT_LEAF;
            sampleStepTime = now;
            break;
        }

        case SAMPLE_WAIT_LEAF:
            if (now - sampleStepTime >= INTER_SENSOR_DELAY_MS) {
                sampleState = SAMPLE_READ_LEAF;
            }
            break;

        case SAMPLE_READ_LEAF: {
            uint16_t rawLeaf = analogRead(LEAF_SENSOR_PIN);
            buildPayload.leaf_wetness = rawLeaf;
            if (rawLeaf > 0) buildStatus |= 0x04;   // bit2 = leaf sensor OK

            buildPayload.sensor_status = buildStatus;
            sampleState = SAMPLE_DONE;
            break;
        }

        case SAMPLE_DONE: {
            /* Atomic copy to volatile cached data */
            portENTER_CRITICAL(&payloadMux);
            memcpy((void*)&cachedPayload, &buildPayload, sizeof(SlavePayload));
            dataReady = true;
            portEXIT_CRITICAL(&payloadMux);

            sampleState = SAMPLE_IDLE;
            break;
        }
    }
}

/* ── I2C Request Handler (ISR context) ─────────────────────────────────── */
void onI2CRequest() {
    portENTER_CRITICAL_ISR(&payloadMux);
    Wire.write((const uint8_t*)&cachedPayload, sizeof(SlavePayload));
    portEXIT_CRITICAL_ISR(&payloadMux);
}
