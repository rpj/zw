#ifndef __ZW_COMMON__H__
#define __ZW_COMMON__H__

#define ZEROWATCH_VER   "0.2.0.4"
#define DEBUG           1

struct ZWAppConfig {
    int brightness;
    int refresh;
    bool debug;
    bool publishLogs;
    bool pauseRefresh;
};

void __haltOrCatchFire();

#define zwassert(cond)                                  \
    do                                                  \
    {                                                   \
        if (!(cond))                                    \
        {                                               \
            Serial.printf("ZASSERT AT %d\n", __LINE__); \
            __haltOrCatchFire();                        \
        }                                               \
    } while (0)

#endif