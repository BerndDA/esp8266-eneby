
#include <Config.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

void ConfigManager::save(Config* config)
{
    DynamicJsonDocument json(512);
    json["mqtt_server"] = config->mqtt_server;
    json["username"] = config->username;
    json["password"] = config->password;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile)
    {
        return;
    }

    serializeJson(json, configFile);
    configFile.close();
}

void ConfigManager::load(Config* config)
{
    if (LittleFS.begin())
    {

        if (LittleFS.exists("/config.json"))
        {
            File configFile = LittleFS.open("/config.json", "r");

            if (configFile)
            {
                const size_t size = configFile.size();
                std::unique_ptr<char[]> buf(new char[size]);

                configFile.readBytes(buf.get(), size);
                DynamicJsonDocument json(512);

                if (DeserializationError::Ok == deserializeJson(json, buf.get()))
                {
                    strcpy(config->mqtt_server, json["mqtt_server"]);
                    strcpy(config->username, json["username"]);
                    strcpy(config->password, json["password"]);
                }
            }
        }
    }
}
