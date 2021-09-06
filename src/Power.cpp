#include <Power.h>

Power::Power(uint8_t pinPwr, uint8_t pinLed)
{
    _pinPwr = pinPwr;
    _pinLed = pinLed;
}

void Power::toggle()
{
    pinMode(_pinPwr, OUTPUT);
    digitalWrite(_pinPwr, 0);
    delay(200);
    pinMode(_pinPwr, INPUT);
}

bool Power::isPowered()
{
    return digitalRead(_pinLed);
}

void Power::on()
{
    if (isPowered())
        return;
    toggle();
}

void Power::off()
{
    if (!isPowered())
        return;
    toggle();
}