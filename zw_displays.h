#ifndef __ZW_DISPLAYS__H__
#define __ZW_DISPLAYS__H__

#include <TM1637Display.h>
#include <Arduino.h>
#include <functional>

#define LED_BLTIN_H LOW
#define LED_BLTIN_L HIGH
#define LED_BLTIN 2
#define LAT_FUNC micros

class DisplaySpec;

struct InfoSpec
{
    const char *listKey;
    int startIdx;
    int endIdx;
    double lastTs;
    int lastVal;
    std::function<int(int)> adjFunc;
    std::function<void(DisplaySpec *)> dispFunc;
};

struct DisplaySpec
{
    int clockPin;
    int dioPin;
    TM1637Display *disp;
    InfoSpec spec;
};

DisplaySpec *zwdisplayInit(String &hostname);

void updateDisplay(DisplaySpec *disp);

void blink(int d = 50);

void runAnimation(TM1637Display *d, String animation, bool cE = false, int s = 0);

void demoMode(DisplaySpec* displayListStart);

#define EXEC_ALL_DISPS(DISPLIST_START, EXEC_ME)                                                      \
    do                                                                                               \
    {                                                                                                \
        for (DisplaySpec *walk = DISPLIST_START; walk->clockPin != -1 && walk->dioPin != -1; walk++) \
            walk->disp->EXEC_ME;                                                                     \
    } while (0)

#endif
