#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>

#include "Config.h"

#define PIN_PWR D3 // GPIO 16 (has a pulldown res, the others have a pullup)
#define PIN_LED D0
#define PIN_VOLUP D7   // VOL+
#define PIN_VOLDOWN D6 // VOL-

uint8_t mqttRetryCounter = 0;
bool powered = false;

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", Config::mqtt_server, sizeof(Config::mqtt_server));
WiFiManagerParameter custom_mqtt_user("user", "MQTT username", Config::username, sizeof(Config::username));
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", Config::password, sizeof(Config::password));

uint32_t lastMqttConnectionAttempt = 0;
const uint16_t mqttConnectionInterval = 60000; // 1 minute = 60 seconds = 60000 milliseconds

uint32_t statusPublishPreviousMillis = 0;
const uint16_t statusPublishInterval = 30000; // 30 seconds = 30000 milliseconds

uint32_t pwrCheckPreviousMillis = 0;
const uint16_t pwrCheckInterval = 1000;

char identifier[24];
#define FIRMWARE_PREFIX "esp8266-eneby"
#define AVAILABILITY_ONLINE "online"
#define AVAILABILITY_OFFLINE "offline"

char MQTT_TOPIC_AVAILABILITY[128];
char MQTT_TOPIC_STATE[128];
char MQTT_TOPIC_COMMAND[128];

char MQTT_TOPIC_AUTOCONF_WIFI_SENSOR[128];
char MQTT_TOPIC_AUTOCONF_PM25_SENSOR[128];

bool shouldSaveConfig = false;

uint16_t currentVol = 100;
bool publishVol = false;
uint8_t volPattern = 0;
char patterns[] = {0b00, 0b01, 0b11, 0b10};
uint8_t patternIdx = 0;

void saveConfigCallback()
{
    shouldSaveConfig = true;
}

uint8_t getEncoderIdx()
{
    pinMode(PIN_VOLUP, INPUT);
    pinMode(PIN_VOLDOWN, INPUT);
    bool vol1 = digitalRead(PIN_VOLUP);
    bool vol2 = digitalRead(PIN_VOLDOWN);
    uint8_t p = vol1 + vol2 * 2;
    if (p == patterns[0])
        return 0;
    return 2;
}

void setupWifi()
{
    wifiManager.setDebugOutput(false);
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);

    WiFi.hostname(identifier);
    wifiManager.autoConnect(identifier);
    //wifiManager.startConfigPortal(identifier);
    mqttClient.setClient(wifiClient);

    strcpy(Config::mqtt_server, custom_mqtt_server.getValue());
    strcpy(Config::username, custom_mqtt_user.getValue());
    strcpy(Config::password, custom_mqtt_pass.getValue());

    if (shouldSaveConfig)
    {
        Config::save();
    }
    else
    {
        // For some reason, the read values get overwritten in this function
        // To combat this, we just reload the config
        // This is most likely a logic error which could be fixed otherwise
        Config::load();
    }
}

void resetWifiSettingsAndReboot()
{
    wifiManager.resetSettings();
    delay(3000);
    ESP.reset();
}

void publishAutoConfig()
{
    char mqttPayload[2048];
    DynamicJsonDocument device(256);
    DynamicJsonDocument autoconfPayload(1024);
    StaticJsonDocument<64> identifiersDoc;
    JsonArray identifiers = identifiersDoc.to<JsonArray>();

    identifiers.add(identifier);

    device["identifiers"] = identifiers;
    device["manufacturer"] = "Ikea";
    device["model"] = "VINDRIKTNING";
    device["name"] = identifier;
    device["sw_version"] = "2021.08.0";

    autoconfPayload["device"] = device.as<JsonObject>();
    autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
    autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
    autoconfPayload["name"] = identifier + String(" WiFi");
    autoconfPayload["value_template"] = "{{value_json.wifi.rssi}}";
    autoconfPayload["unique_id"] = identifier + String("_wifi");
    autoconfPayload["unit_of_measurement"] = "dBm";
    autoconfPayload["json_attributes_topic"] = MQTT_TOPIC_STATE;
    autoconfPayload["json_attributes_template"] = "{\"ssid\": \"{{value_json.wifi.ssid}}\", \"ip\": \"{{value_json.wifi.ip}}\"}";
    autoconfPayload["icon"] = "mdi:wifi";

    serializeJson(autoconfPayload, mqttPayload);
    mqttClient.publish(&MQTT_TOPIC_AUTOCONF_WIFI_SENSOR[0], &mqttPayload[0], true);

    autoconfPayload.clear();

    autoconfPayload["device"] = device.as<JsonObject>();
    autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
    autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
    autoconfPayload["name"] = identifier + String(" PM 2.5");
    autoconfPayload["unit_of_measurement"] = "μg/m³";
    autoconfPayload["value_template"] = "{{value_json.power}}";
    autoconfPayload["unique_id"] = identifier + String("_pm25");
    autoconfPayload["icon"] = "mdi:air-filter";

    serializeJson(autoconfPayload, mqttPayload);
    mqttClient.publish(&MQTT_TOPIC_AUTOCONF_PM25_SENSOR[0], &mqttPayload[0], true);

    autoconfPayload.clear();
}

void mqttReconnect()
{
    for (uint8_t attempt = 0; attempt < 3; ++attempt)
    {
        if (mqttClient.connect(identifier, Config::username, Config::password, MQTT_TOPIC_AVAILABILITY, 1, true, AVAILABILITY_OFFLINE))
        {
            mqttClient.publish(MQTT_TOPIC_AVAILABILITY, AVAILABILITY_ONLINE, true);
            publishAutoConfig();

            // Make sure to subscribe after polling the status so that we never execute commands with the default data
            mqttClient.subscribe(MQTT_TOPIC_COMMAND);
            break;
        }
        delay(5000);
    }
}

bool isMqttConnected()
{
    return mqttClient.connected();
}

void publishState()
{
    DynamicJsonDocument wifiJson(192);
    DynamicJsonDocument stateJson(604);
    char payload[256];

    wifiJson["ssid"] = WiFi.SSID();
    wifiJson["ip"] = WiFi.localIP().toString();
    wifiJson["rssi"] = WiFi.RSSI();

    stateJson["power"] = powered ? "on" : "off";
    stateJson["vol"] = currentVol;
    stateJson["pattern"] = String(volPattern, BIN);
    stateJson["wifi"] = wifiJson.as<JsonObject>();

    serializeJson(stateJson, payload);
    mqttClient.publish(&MQTT_TOPIC_STATE[0], &payload[0], true);
}

bool isPowered()
{

    return digitalRead(PIN_LED);
}

void togglePower()
{
    pinMode(PIN_PWR, OUTPUT);
    digitalWrite(PIN_PWR, 0);
    delay(200);
    //digitalWrite(PIN_PWR,255);
    //delay(200);
    pinMode(PIN_PWR, INPUT);
}

void powerOn()
{
    if (isPowered())
        return;
    togglePower();
}

void powerOff()
{
    if (!isPowered())
        return;
    togglePower();
}

void stepVolume(bool reverse)
{
    digitalWrite(reverse ? PIN_VOLDOWN : PIN_VOLUP, HIGH);
    delay(20);
    digitalWrite(reverse ? PIN_VOLUP : PIN_VOLDOWN, HIGH);
    delay(20);
    digitalWrite(reverse ? PIN_VOLDOWN : PIN_VOLUP, LOW);
    delay(20);
    digitalWrite(reverse ? PIN_VOLUP : PIN_VOLDOWN, LOW);
    delay(20);
}

void setVolume(uint16_t absolute)
{

    long diff = absolute - currentVol;
    if (diff != 0)
    {
        pinMode(PIN_VOLUP, OUTPUT);
        pinMode(PIN_VOLDOWN, OUTPUT);
        for (uint16_t i = 0; i < abs(diff); i++)
            stepVolume(diff < 0);
        pinMode(PIN_VOLUP, INPUT);
        pinMode(PIN_VOLDOWN, INPUT);
        currentVol = absolute;
    }
}

void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
    char payloadBuffer[50];

    strncpy(&payloadBuffer[0], (char *)payload, length);
    payloadBuffer[length] = '\0';
    String payloads = String((char *)payloadBuffer);
    Serial.print("MQTT msg: ");
    Serial.println(payloads);
    if (payloads == "on")
        powerOn();
    else if (payloads == "off")
        powerOff();
    else if (payloads.startsWith("vol"))
    {
        uint16_t vol = payloads.substring(3).toInt();
        setVolume(vol);
        publishVol = true;
    }
    else if (payloads == "reset")
        resetWifiSettingsAndReboot();
}

void setupOTA()
{
    ArduinoOTA.onStart([]()
                       { Serial.println("Start"); });
    ArduinoOTA.onEnd([]()
                     { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
    ArduinoOTA.onError([](ota_error_t error)
                       {
                           Serial.printf("Error[%u]: ", error);
                           if (error == OTA_AUTH_ERROR)
                           {
                               Serial.println("Auth Failed");
                           }
                           else if (error == OTA_BEGIN_ERROR)
                           {
                               Serial.println("Begin Failed");
                           }
                           else if (error == OTA_CONNECT_ERROR)
                           {
                               Serial.println("Connect Failed");
                           }
                           else if (error == OTA_RECEIVE_ERROR)
                           {
                               Serial.println("Receive Failed");
                           }
                           else if (error == OTA_END_ERROR)
                           {
                               Serial.println("End Failed");
                           }
                       });

    ArduinoOTA.setHostname(identifier);

    // This is less of a security measure and more a accidential flash prevention
    //ArduinoOTA.setPassword(identifier);
    ArduinoOTA.begin();
}

void setup()
{
    Serial.begin(115200);

    Serial.println("\n");
    Serial.println("Hello from esp8266-eneby");
    Serial.printf("Core Version: %s\n", ESP.getCoreVersion().c_str());
    Serial.printf("Boot Version: %u\n", ESP.getBootVersion());
    Serial.printf("Boot Mode: %u\n", ESP.getBootMode());
    Serial.printf("CPU Frequency: %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Reset reason: %s\n", ESP.getResetReason().c_str());
    //delay(3000);

    snprintf(identifier, sizeof(identifier), "ENEBY-%X", ESP.getChipId());
    snprintf(MQTT_TOPIC_AVAILABILITY, 127, "%s/%s/status", FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_STATE, 127, "%s/%s/state", FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_COMMAND, 127, "%s/%s/command", FIRMWARE_PREFIX, identifier);

    snprintf(MQTT_TOPIC_AUTOCONF_PM25_SENSOR, 127, "homeassistant/sensor/%s/%s_pm25/config", FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_AUTOCONF_WIFI_SENSOR, 127, "homeassistant/sensor/%s/%s_wifi/config", FIRMWARE_PREFIX, identifier);

    WiFi.hostname(identifier);

    Config::load();

    setupWifi();
    setupOTA();
    mqttClient.setServer(Config::mqtt_server, 1883);
    mqttClient.setKeepAlive(10);
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(mqttCallback);

    Serial.printf("Hostname: %s\n", identifier);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    pinMode(PIN_LED, INPUT_PULLDOWN_16);
    patternIdx = getEncoderIdx();

    Serial.println("-- Current GPIO Configuration --");
    Serial.printf("PIN_LED: %d\n", PIN_LED);
    Serial.printf("PIN_PWR: %d\n", PIN_PWR);
    Serial.printf("PIN_VOLUP: %d\n", PIN_VOLUP);
    Serial.printf("PIN_VOLDOWN: %d\n", PIN_VOLDOWN);

    mqttReconnect();
}

void loop()
{
    ArduinoOTA.handle();
    //SerialCom::handleUart(state);
    mqttClient.loop();

    const uint32_t currentMillis = millis();
    if (currentMillis - statusPublishPreviousMillis >= statusPublishInterval)
    {
        statusPublishPreviousMillis = currentMillis;
        printf("Publish state\n");
        publishState();
    }

    if (currentMillis - pwrCheckPreviousMillis >= pwrCheckInterval)
    {
        pwrCheckPreviousMillis = currentMillis;
        bool pwr = isPowered();
        if (pwr != powered)
        {
            powered = pwr;
            currentVol = 0;
            publishState();
            Serial.printf("POWER: %d\n", isPowered());
        }
        else if (publishVol)
        {
            publishState();
        }
        publishVol = false;
    }

    if (!mqttClient.connected() && currentMillis - lastMqttConnectionAttempt >= mqttConnectionInterval)
    {
        lastMqttConnectionAttempt = currentMillis;
        printf("Reconnect mqtt\n");
        mqttReconnect();
    }

    bool vol1 = digitalRead(PIN_VOLUP);
    bool vol2 = digitalRead(PIN_VOLDOWN);
    uint8_t p = vol1 + vol2 * 2;
    if (p != volPattern)
    {
        volPattern = p;
        publishState();
    }
}
