#include <Volume.h>

const char Volume::patterns[] = {0b00, 0b01, 0b11, 0b10};

Volume::Volume(uint8_t pinUp, uint8_t pinDown)
{
    _pinUp = pinUp;
    _pinDown = pinDown;
    disable();
}

void Volume::reset()
{
    _currentVol = 20;
}

void Volume::disable()
{
    _currentVol = 0;
}

uint8_t Volume::getVolume()
{
    return _currentVol;
}

void Volume::stepVol(bool up)
{
    readEncoderIdx();
    setOutputToMatchPattern();
    pinMode(_pinUp, OUTPUT);
    pinMode(_pinDown, OUTPUT);
    for (uint8_t i = 0; i <= 3; i++)
    {
        _currentIdx = _currentIdx + (up ? 1 : -1);
        _currentIdx &= 3;
        setOutputToMatchPattern();
        delay(20);
    }
    pinMode(_pinUp, INPUT);
    pinMode(_pinDown, INPUT);
}

void Volume::volDown()
{
    stepVol(false);
    _currentVol -= 2;
}

void Volume::volUp()
{
    stepVol(true);
    _currentVol += 2;
}

void Volume::setVolume(uint8_t level)
{
    if (level > _currentVol)
    {
        while (level > _currentVol)
        {
            volUp();
        }
    }
    else if (level < _currentVol)
    {
        while (level < _currentVol)
        {
            volDown();
        }
    }
}

void Volume::setOutputToMatchPattern()
{
    digitalWrite(_pinUp, patterns[_currentIdx] & 1);
    digitalWrite(_pinDown, patterns[_currentIdx] & 2);
}

void Volume::readEncoderIdx()
{
    pinMode(_pinUp, INPUT);
    pinMode(_pinDown, INPUT);
    bool vol1 = digitalRead(_pinUp);
    bool vol2 = digitalRead(_pinDown);
    uint8_t p = vol1 + vol2 * 2;
    if (p == patterns[0])
        _currentIdx = 0;
    else
        _currentIdx = 2;
}