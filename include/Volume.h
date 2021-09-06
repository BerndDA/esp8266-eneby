#pragma once
#include <Arduino.h>

class Volume
{
private:
    const static char patterns[];

    uint8_t _pinUp;
    uint8_t _pinDown;
    uint8_t _currentIdx;
    uint8_t _currentVol;

    void readEncoderIdx();
    void setOutputToMatchPattern();
    void stepVol(bool up);

public:
    Volume(uint8_t pinUp, uint8_t pinDown);
    void volUp();
    void volDown();
    void setVolume(uint8_t level);
    void reset();
    void disable();
    uint8_t getVolume();
};
