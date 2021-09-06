#pragma once

struct Config
{
    char mqtt_server[80] = "examplemqtt.tld";
    char username[24] = "";
    char password[24] = "";
};

class ConfigManager
{
public:
    void save(Config *config);
    void load(Config *config);
};
