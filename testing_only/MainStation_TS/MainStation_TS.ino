// MainStation_TS.ino - OOP & SILENT VERSION
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>

// --- Data Structures ---
struct SlavePayloadParent {
    float    soil_temp[5];
    uint16_t leaf_wetness;
    uint16_t rain_count;
    uint8_t  sensor_status;
    uint8_t  padding;
} __attribute__((packed));

struct SensorData {
    float    temp;
    float    humidity;          
    float    soil_temp[5];      
    uint16_t leaf_wetness;      
    uint16_t rain_gauge;        
    uint8_t  security_alert;    
    uint8_t  sensor_status;
} __attribute__((packed));

struct DataPacket {
    uint8_t  magic;             
    uint8_t  type;              
    uint8_t  sourceId;          
    uint8_t  relayControl;
    uint16_t sequenceId;        
    uint32_t timestamp;         
    SensorData data;            
    int16_t  rssi;              
    float    snr;               
    uint16_t crc;               
} __attribute__((packed));

// --- Constants & Config ---
const uint8_t  MAGIC_BYTE = 0xAF;
const uint8_t  PKT_DATA   = 0x01;
const uint8_t  NODE_PARENT = 5;
const uint8_t  NODE_MAIN   = 6;
const long     LORA_FREQ  = 433E6;
const uint8_t  I2C_ESP32_SLAVE = 0x08;

const uint8_t PIN_LORA_CS   = 10;
const uint8_t PIN_LORA_RST  = 9;
const uint8_t PIN_LORA_DIO0 = 2;
const uint8_t PIN_SECURITY  = 3;
const uint8_t PIN_LED_A2    = A2;
const uint8_t PIN_LED_A3    = A3;

const String TS_API_KEY_MAIN = "XS7I6XDUWM6CHG0E";
const String TS_API_KEY_PARENT = "BNCGFLGL32LV7MIJ";
const String SIM_APN = "airtelgprs.com";

enum ChannelTarget { TARGET_MAIN, TARGET_PARENT };

// --- Utility Functions ---
uint16_t calculateCRC(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

// --- Classes ---

class Indicator {
private:
    uint8_t pin;
    unsigned long timer;
public:
    Indicator(uint8_t p) : pin(p), timer(0) {}
    void begin() { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
    void flash() { digitalWrite(pin, HIGH); timer = millis(); }
    void update() { if (timer > 0 && millis() - timer > 200) { digitalWrite(pin, LOW); timer = 0; } }
};

Indicator ledRX(PIN_LED_A2);
Indicator ledTX(PIN_LED_A3);

class GPRSManager {
private:
    enum State { BOOT, BOOT_AT, BOOT_CGATT, BOOT_CONTYPE, BOOT_APN, IDLE, HTTP_TERM_SAFE, HTTP_INIT, HTTP_CID, HTTP_URL, HTTP_ACTION, HTTP_WAIT_RESP, HTTP_TERM };
    State state = BOOT;
    unsigned long timer = 0;
    
    struct UploadJob { ChannelTarget target; SensorData data; };
    static const int MAX_QUEUE = 4;
    UploadJob queue[MAX_QUEUE];
    UploadJob currentJob;
    int head = 0, tail = 0;

    void sendAT(String cmd) { Serial.println(cmd); }

public:
    void begin() { 
        Serial.begin(9600); 
        timer = millis();
    }

    void enqueue(ChannelTarget t, SensorData &d) {
        int nextTail = (tail + 1) % MAX_QUEUE;
        if (nextTail != head) {
            queue[tail] = {t, d};
            tail = nextTail;
        }
    }

    void update() {
        while (Serial.available()) Serial.read(); // Silent flush
        
        unsigned long now = millis();
        switch (state) {
            case BOOT: if (now - timer >= 10000) { sendAT("AT"); state = BOOT_AT; timer = now; } break;
            case BOOT_AT: if (now - timer >= 1000) { sendAT("AT+CGATT=1"); state = BOOT_CGATT; timer = now; } break;
            case BOOT_CGATT: if (now - timer >= 1000) { sendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\""); state = BOOT_CONTYPE; timer = now; } break;
            case BOOT_CONTYPE: if (now - timer >= 1000) { sendAT("AT+SAPBR=3,1,\"APN\",\"" + SIM_APN + "\""); state = BOOT_APN; timer = now; } break;
            case BOOT_APN: if (now - timer >= 1000) { sendAT("AT+SAPBR=1,1"); state = IDLE; timer = now; } break;
            case IDLE:
                if (head != tail) {
                    currentJob = queue[head];
                    head = (head + 1) % MAX_QUEUE;
                    state = HTTP_TERM_SAFE;
                    timer = now;
                }
                break;
            case HTTP_TERM_SAFE: if (now - timer >= 1000) { sendAT("AT+HTTPTERM"); state = HTTP_INIT; timer = now; } break;
            case HTTP_INIT: if (now - timer >= 4000) { sendAT("AT+HTTPINIT"); state = HTTP_CID; timer = now; } break;
            case HTTP_CID: if (now - timer >= 1000) { sendAT("AT+HTTPPARA=\"CID\",1"); state = HTTP_URL; timer = now; } break;
            case HTTP_URL:
                if (now - timer >= 1000) {
                    SensorData &d = currentJob.data;
                    String url = "AT+HTTPPARA=\"URL\",\"http://api.thingspeak.com/update?api_key=";
                    url += (currentJob.target == TARGET_MAIN) ? TS_API_KEY_MAIN : TS_API_KEY_PARENT;
                    url += "&field1=" + String(isnan(d.temp) ? 0.0 : d.temp, 2);
                    url += "&field2=" + String(isnan(d.humidity) ? 0.0 : d.humidity, 2);
                    url += "&field3=" + String(isnan(d.soil_temp[0]) ? 0.0 : d.soil_temp[0], 2);
                    url += "&field4=" + String(isnan(d.soil_temp[1]) ? 0.0 : d.soil_temp[1], 2);
                    url += "&field5=" + String(isnan(d.soil_temp[2]) ? 0.0 : d.soil_temp[2], 2);
                    url += "&field6=" + String(isnan(d.soil_temp[3]) ? 0.0 : d.soil_temp[3], 2);
                    url += "&field7=" + String(d.leaf_wetness);
                    url += "&field8=" + String(d.rain_gauge) + "\"";
                    sendAT(url);
                    state = HTTP_ACTION; timer = now;
                }
                break;
            case HTTP_ACTION: if (now - timer >= 1000) { ledTX.flash(); sendAT("AT+HTTPACTION=0"); state = HTTP_WAIT_RESP; timer = now; } break;
            case HTTP_WAIT_RESP: if (now - timer >= 15000) { sendAT("AT+HTTPTERM"); state = HTTP_TERM; timer = now; } break;
            case HTTP_TERM: if (now - timer >= 1000) { state = IDLE; } break;
        }
    }
};

class SensorHub {
private:
    Adafruit_AHTX0 aht;
    bool ahtConnected = false;
    unsigned long lastRead = 0;
    bool isFirst = true;
    GPRSManager* gprs;

public:
    SensorHub(GPRSManager* g) : gprs(g) {}

    void begin() {
        Wire.begin();
        pinMode(PIN_SECURITY, INPUT_PULLUP);
        ahtConnected = aht.begin();
    }

    void update() {
        if (millis() - lastRead >= 60000UL || (isFirst && millis() >= 30000UL)) {
            isFirst = false;
            lastRead = millis();
            
            SensorData data = {0};
            if (!ahtConnected) ahtConnected = aht.begin();

            if (ahtConnected) {
                sensors_event_t h, t;
                aht.getEvent(&h, &t);
                if (!isnan(t.temperature) && !isnan(h.relative_humidity)) {
                    data.temp = t.temperature;
                    data.humidity = h.relative_humidity;
                } else ahtConnected = false;
            }

            SlavePayloadParent slaveData = {0};
            uint8_t reqSize = sizeof(SlavePayloadParent);
            Wire.requestFrom((uint8_t)I2C_ESP32_SLAVE, reqSize);
            if (Wire.available() == reqSize) {
                uint8_t* ptr = (uint8_t*)&slaveData;
                for (size_t i = 0; i < reqSize; i++) ptr[i] = Wire.read();
                memcpy(data.soil_temp, slaveData.soil_temp, sizeof(data.soil_temp));
                data.leaf_wetness = slaveData.leaf_wetness;
                data.rain_gauge = slaveData.rain_count;
            }

            data.security_alert = (digitalRead(PIN_SECURITY) == LOW) ? 1 : 0;
            gprs->enqueue(TARGET_MAIN, data);
        }
    }
};

class LoRaNetwork {
private:
    GPRSManager* gprs;
public:
    LoRaNetwork(GPRSManager* g) : gprs(g) {}

    void begin() {
        LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);
        if (!LoRa.begin(LORA_FREQ)) while(1);
        LoRa.setTxPower(20);
        LoRa.setSpreadingFactor(9);
        LoRa.setSignalBandwidth(125E3);
        LoRa.setCodingRate4(5);
        LoRa.setSyncWord(MAGIC_BYTE);
        LoRa.enableCrc();
    }

    void listen() {
        int sz = LoRa.parsePacket();
        if (sz == sizeof(DataPacket)) {
            DataPacket pkt;
            uint8_t* ptr = (uint8_t*)&pkt;
            for (size_t i = 0; i < sz; i++) ptr[i] = LoRa.read();
            
            uint16_t expectedCrc = calculateCRC((const uint8_t*)&pkt, sizeof(DataPacket) - sizeof(uint16_t));
            if (pkt.magic == MAGIC_BYTE && pkt.type == PKT_DATA && pkt.sourceId == NODE_PARENT && pkt.crc == expectedCrc) {
                ledRX.flash();
                gprs->enqueue(TARGET_PARENT, pkt.data);
            }
        } else if (sz > 0) {
            while (LoRa.available()) LoRa.read();
        }
    }
};

// --- Instances ---
GPRSManager gsm;
SensorHub sensors(&gsm);
LoRaNetwork lora(&gsm);

void setup() {
    delay(2000); // Hardware stabilization
    ledRX.begin();
    ledTX.begin();
    gsm.begin();
    sensors.begin();
    lora.begin();
    
    // Boot sequence flash
    ledRX.flash(); ledTX.flash();
}

void loop() {
    ledRX.update();
    ledTX.update();
    gsm.update();
    lora.listen();
    sensors.update();
}