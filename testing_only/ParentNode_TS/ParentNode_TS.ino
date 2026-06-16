// ParentNode_TS.ino - OOP & SILENT VERSION
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

// --- Utility ---
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

class LoRaTransmitter {
private:
    uint16_t seqCounter = 0;
public:
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

    void sendData(SensorData& sData) {
        DataPacket pkt = {0};
        pkt.magic = MAGIC_BYTE;
        pkt.type = PKT_DATA;
        pkt.sourceId = NODE_PARENT;
        pkt.sequenceId = ++seqCounter;
        pkt.relayControl = ((NODE_MAIN & 0x0F) << 4) | (NODE_PARENT & 0x0F);
        pkt.data = sData;
        pkt.crc = calculateCRC((const uint8_t*)&pkt, sizeof(DataPacket) - sizeof(uint16_t));

        ledTX.flash();
        LoRa.beginPacket();
        LoRa.write((uint8_t*)&pkt, sizeof(DataPacket));
        LoRa.endPacket();
    }
};

class ParentHub {
private:
    Adafruit_AHTX0 aht;
    LoRaTransmitter* lora;
    bool ahtConnected = false;
    unsigned long lastSend = 0;
    bool isFirst = true;

public:
    ParentHub(LoRaTransmitter* l) : lora(l) {}

    void begin() {
        Wire.begin();
        pinMode(PIN_SECURITY, INPUT_PULLUP);
        ahtConnected = aht.begin();
    }

    void update() {
        if (isFirst || millis() - lastSend >= 60000UL) {
            lastSend = millis();
            isFirst = false;
            
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
            lora->sendData(data);
            ledRX.flash(); // Indicate successful read & package
        }
    }
};

// --- Instances ---
LoRaTransmitter loraTX;
ParentHub hub(&loraTX);

void setup() {
    delay(2000); // Hardware stabilization
    ledRX.begin();
    ledTX.begin();
    loraTX.begin();
    hub.begin();
    
    // Boot sequence flash
    ledRX.flash(); ledTX.flash();
}

void loop() {
    ledRX.update();
    ledTX.update();
    hub.update();
}