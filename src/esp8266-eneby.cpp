#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>

#include "Config.h"
#include "Volume.h"
#include "Power.h"

#define PIN_PWR D3 // GPIO 16 (has a pulldown res, the others have a pullup)
#define PIN_LED D0
#define PIN_VOLUP D7   // VOL+
#define PIN_VOLDOWN D6 // VOL-

struct PublishValues
{
    bool powered;
    uint8_t volume;
};
bool operator==(const PublishValues &lhs, const PublishValues &rhs)
{
    return lhs.powered == rhs.powered && lhs.volume == rhs.volume;
};
bool operator!=(const PublishValues &lhs, const PublishValues &rhs)
{
    return !(lhs == rhs);
};

uint8_t mqttRetryCounter = 0;
PublishValues pubVal;

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient;
Config config;
ConfigManager cfManager;
Volume vol = Volume(PIN_VOLUP, PIN_VOLDOWN);
Power pwr = Power(PIN_PWR, PIN_LED); 

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", config.mqtt_server, sizeof(config.mqtt_server));
WiFiManagerParameter custom_mqtt_user("user", "MQTT username", config.username, sizeof(config.username));
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", config.password, sizeof(config.password));

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

void saveConfigCallback()
{
    shouldSaveConfig = true;
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

    strcpy(config.mqtt_server, custom_mqtt_server.getValue());
    strcpy(config.username, custom_mqtt_user.getValue());
    strcpy(config.password, custom_mqtt_pass.getValue());

    if (shouldSaveConfig)
    {
        cfManager.save(&config);
    }
    else
    {
        // For some reason, the read values get overwritten in this function
        // To combat this, we just reload the config
        // This is most likely a logic error which could be fixed otherwise
        cfManager.load(&config);
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
    device["model"] = "ENEBY";
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
    autoconfPayload["name"] = identifier + String(" Control");
    autoconfPayload["unit_of_measurement"] = "%";
    autoconfPayload["value_template"] = "{{value_json.power}}";
    autoconfPayload["unique_id"] = identifier + String("_control");
    autoconfPayload["icon"] = "mdi:control";

    serializeJson(autoconfPayload, mqttPayload);
    mqttClient.publish(&MQTT_TOPIC_AUTOCONF_PM25_SENSOR[0], &mqttPayload[0], true);

    autoconfPayload.clear();
}

void mqttReconnect()
{
    for (uint8_t attempt = 0; attempt < 3; ++attempt)
    {
        if (mqttClient.connect(identifier, config.username, config.password, MQTT_TOPIC_AVAILABILITY, 1, true, AVAILABILITY_OFFLINE))
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

void publishState(PublishValues *val)
{
    DynamicJsonDocument wifiJson(192);
    DynamicJsonDocument stateJson(604);
    char payload[256];

    wifiJson["ssid"] = WiFi.SSID();
    wifiJson["ip"] = WiFi.localIP().toString();
    wifiJson["rssi"] = WiFi.RSSI();

    stateJson["power"] = val->powered ? "on" : "off";
    stateJson["volume"] = val->volume;
    stateJson["wifi"] = wifiJson.as<JsonObject>();
    stateJson["heap"] = ESP.getFreeHeap();
    serializeJson(stateJson, payload);
    mqttClient.publish(&MQTT_TOPIC_STATE[0], &payload[0], true);
}

void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
    char payloadBuffer[50];

    strncpy(&payloadBuffer[0], (char *)payload, length);
    payloadBuffer[length] = '\0';
    String payloads = String((char *)payloadBuffer);
    String topicStr = String(topic);
    Serial.print("MQTT msg: ");
    Serial.println(payloads);
    if (topicStr.endsWith("power") && payloads == "on")
        pwr.on();
    else if (topicStr.endsWith("power") && payloads == "off")
        pwr.off();
    else if (topicStr.endsWith("volume") && payloads.startsWith("up"))
        vol.volUp();
    else if (topicStr.endsWith("volume") && payloads.startsWith("down"))
        vol.volDown();
    else if (topicStr.endsWith("volume"))
    {
        uint8_t newVol = payloads.toInt();
        if (newVol == 0)
            pwr.off();
        else
            vol.setVolume(newVol);
    }
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
    snprintf(MQTT_TOPIC_COMMAND, 127, "%s/%s/command/#", FIRMWARE_PREFIX, identifier);

    snprintf(MQTT_TOPIC_AUTOCONF_PM25_SENSOR, 127, "homeassistant/sensor/%s/%s_device/config", FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_AUTOCONF_WIFI_SENSOR, 127, "homeassistant/sensor/%s/%s_wifi/config", FIRMWARE_PREFIX, identifier);

    WiFi.hostname(identifier);

    cfManager.load(&config);

    setupWifi();
    setupOTA();
    mqttClient.setServer(config.mqtt_server, 1883);
    mqttClient.setKeepAlive(10);
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(mqttCallback);

    Serial.printf("Hostname: %s\n", identifier);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    pinMode(PIN_LED, INPUT_PULLDOWN_16);

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
    mqttClient.loop();
    PublishValues newValues;

    const uint32_t currentMillis = millis();
    if (currentMillis - statusPublishPreviousMillis >= statusPublishInterval)
    {
        newValues.powered = pwr.isPowered();
        newValues.volume = vol.getVolume();
        statusPublishPreviousMillis = currentMillis;
        printf("Publish state\n");
        publishState(&newValues);
        pubVal = newValues;
    }

    if (currentMillis - pwrCheckPreviousMillis >= pwrCheckInterval)
    {
        pwrCheckPreviousMillis = currentMillis;
        newValues.powered = pwr.isPowered();
        newValues.volume = vol.getVolume();
        if (pubVal.powered != newValues.powered)
        {
            if (!newValues.powered)
                vol.disable();
            else
                vol.reset();
            newValues.volume = vol.getVolume();
        }
        if (newValues != pubVal)
        {
            pubVal = newValues;
            publishState(&pubVal);
        }
    }

    if (!mqttClient.connected() && currentMillis - lastMqttConnectionAttempt >= mqttConnectionInterval)
    {
        lastMqttConnectionAttempt = currentMillis;
        printf("Reconnect mqtt\n");
        mqttReconnect();
    }
}
