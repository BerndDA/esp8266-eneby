#pragma once
#include <Arduino.h>

class Power
{
private:
    uint8_t _pinPwr;
    uint8_t _pinLed;

public:
    Power(uint8_t pinPwr, uint8_t pinLed);
    void on();
    void off();
    void toggle();
    bool isPowered();
};
