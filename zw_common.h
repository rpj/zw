#ifndef __ZW_COMMON__H__
#define __ZW_COMMON__H__

#define ZEROWATCH_VER "0.2.3.8"
#define DEBUG 1

struct ZWAppConfig
{
    int brightness;
    int refresh;
    bool debug;
    bool publishLogs;
    bool pauseRefresh;
    bool deepSleepMode;
};

void __haltOrCatchFire();

#define zwassert(cond)                                                                      \
    do                                                                                      \
    {                                                                                       \
        if (!(cond))                                                                        \
        {                                                                                   \
            Serial.printf("ZWASSERT IN %s() AT %s:%d\n", __FUNCTION__, __FILE__, __LINE__); \
            __haltOrCatchFire();                                                            \
        }                                                                                   \
    } while (0)

#endif
