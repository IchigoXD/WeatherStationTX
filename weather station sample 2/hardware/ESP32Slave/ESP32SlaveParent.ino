/*
 * ============================================================================
 *  ESP32 Sensor Slave — Parent Station Variant
 * ============================================================================
 *  Reads: 5× DS18B20, capacitive leaf wetness, tipping-bucket rain gauge
 *  Communicates: I2C slave to ATmega328P master
 *
 *  Differences from child variant:
 *    - Also reads rain gauge (interrupt-driven tipping bucket counter)
 *    - Reports rain count since last read (auto-resets on I2C request)
 *    - Uses portMUX for thread-safe access to shared data
 *    - Non-blocking sensor read state machine with millis() delays
 * ============================================================================
 */

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>

/* ── Pin Definitions ───────────────────────────────────────────────────── */
#define ONE_WIRE_BUS      4         // DS18B20 data line
#define LEAF_SENSOR_PIN   34        // Capacitive leaf wetness (analog)
#define RAIN_GAUGE_PIN    35        // Tipping bucket reed switch (interrupt)
#define I2C_SDA           21
#define I2C_SCL           22
#define SLAVE_ADDR        0x08      // Must match I2C_ESP32_SLAVE in Config.h

/* ── Payload Structure (MUST match SlavePayloadParent in Protocol.h) ───── */
/*
 * If Protocol.h changes SlavePayloadParent, update this struct to match.
 * The static_assert below will catch any size mismatch at compile time.
 */
struct SlavePayload {
    float    soil_temp[5];          // 5 depth probes (20 bytes)
    uint16_t leaf_wetness;          // ADC value (2 bytes)
    uint16_t rain_count;            // Tip count since last read (2 bytes)
    uint8_t  sensor_status;         // Bitmask
    uint8_t  padding;               // Alignment
} __attribute__((packed));          // Total: 26 bytes
static_assert(sizeof(SlavePayload) == 26, "SlavePayload must be 26 bytes (match SlavePayloadParent in Protocol.h)");

/* ── Globals ───────────────────────────────────────────────────────────── */
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

volatile SlavePayload cachedPayload;
volatile uint16_t tipCounter = 0;       // Accumulated rain tips
volatile unsigned long lastTipTime = 0; // Debounce
unsigned long lastSampleTime = 0;
const unsigned long SAMPLE_INTERVAL = 2000;
const unsigned long TIP_DEBOUNCE_MS = 200;  // Reed switch debounce

/* Mutex for thread-safe payload + tipCounter access */
portMUX_TYPE payloadMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t sensorCount = 0;

/* ── Sub-state for non-blocking sensor reads ───────────────────────────── */
enum SampleState : uint8_t {
    SAMPLE_IDLE,
    SAMPLE_DS18B20_REQUEST,
    SAMPLE_WAIT_DS18B20,
    SAMPLE_READ_DS18B20,
    SAMPLE_WAIT_LEAF,
    SAMPLE_READ_LEAF,
    SAMPLE_WAIT_RAIN,
    SAMPLE_READ_RAIN,
    SAMPLE_DONE
};

SampleState sampleState = SAMPLE_IDLE;
unsigned long sampleStepTime = 0;
SlavePayload buildPayload;
uint8_t buildStatus = 0;

const unsigned long DS18B20_CONV_MS = 800;
const unsigned long INTER_SENSOR_DELAY_MS = 50;

/* ── Forward Declarations ──────────────────────────────────────────────── */
void IRAM_ATTR onRainTip();
void onI2CRequest();

/* ── Setup ─────────────────────────────────────────────────────────────── */
void setup() {
    Serial.begin(115200);
    Serial.println(F("[ESP32-Parent] Sensor Slave Initializing..."));

    /* DS18B20 */
    ds18b20.begin();
    sensorCount = ds18b20.getDeviceCount();
    ds18b20.setWaitForConversion(false);
    Serial.print(F("[ESP32-Parent] DS18B20 sensors found: "));
    Serial.println(sensorCount);

    /* Rain gauge — interrupt on falling edge (reed switch closes on tip) */
    pinMode(RAIN_GAUGE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(RAIN_GAUGE_PIN), onRainTip, FALLING);

    /* Initialize payload */
    memset((void*)&cachedPayload, 0, sizeof(SlavePayload));
    memset(&buildPayload, 0, sizeof(SlavePayload));

    /* I2C Slave */
    Wire.begin(SLAVE_ADDR);
    Wire.onRequest(onI2CRequest);

    /* Watchdog */
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

    Serial.println(F("[ESP32-Parent] Ready."));
}

/* ── Main Loop ─────────────────────────────────────────────────────────── */
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
            bool anyOK = false;
            for (uint8_t i = 0; i < 5; i++) {
                if (i < sensorCount) {
                    float t = ds18b20.getTempCByIndex(i);
                    if (t == DEVICE_DISCONNECTED_C || isnan(t) || t < -55.0f || t > 125.0f) {
                        buildPayload.soil_temp[i] = 0.0f;
                    } else {
                        buildPayload.soil_temp[i] = t;
                        anyOK = true;
                    }
                } else {
                    buildPayload.soil_temp[i] = 0.0f;
                }
            }
            if (anyOK) buildStatus |= 0x02;

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
            if (rawLeaf > 0) buildStatus |= 0x04;

            sampleState = SAMPLE_WAIT_RAIN;
            sampleStepTime = now;
            break;
        }

        case SAMPLE_WAIT_RAIN:
            if (now - sampleStepTime >= INTER_SENSOR_DELAY_MS) {
                sampleState = SAMPLE_READ_RAIN;
            }
            break;

        case SAMPLE_READ_RAIN: {
            /* Read rain counter atomically */
            portENTER_CRITICAL(&payloadMux);
            buildPayload.rain_count = tipCounter;
            /* Don't reset here — reset on I2C read to avoid losing tips */
            portEXIT_CRITICAL(&payloadMux);
            if (buildPayload.rain_count > 0) buildStatus |= 0x08;

            buildPayload.sensor_status = buildStatus;
            sampleState = SAMPLE_DONE;
            break;
        }

        case SAMPLE_DONE: {
            /* Atomic copy to volatile cached payload */
            portENTER_CRITICAL(&payloadMux);
            memcpy((void*)&cachedPayload, &buildPayload, sizeof(SlavePayload));
            portEXIT_CRITICAL(&payloadMux);

            sampleState = SAMPLE_IDLE;
            break;
        }
    }
}

/* ── Rain Gauge ISR (IRAM for ESP32) ───────────────────────────────────── */
void IRAM_ATTR onRainTip() {
    unsigned long now = millis();
    if (now - lastTipTime > TIP_DEBOUNCE_MS) {
        portENTER_CRITICAL_ISR(&payloadMux);
        tipCounter++;
        lastTipTime = now;  // EP-02: inside critical section to prevent preemption
        portEXIT_CRITICAL_ISR(&payloadMux);
    }
}

/* ── I2C Request Handler (ISR context) ─────────────────────────────────── */
void onI2CRequest() {
    portENTER_CRITICAL_ISR(&payloadMux);
    Wire.write((const uint8_t*)&cachedPayload, sizeof(SlavePayload));
    /* Reset rain counter atomically after master reads it */
    tipCounter = 0;
    portEXIT_CRITICAL_ISR(&payloadMux);
}
