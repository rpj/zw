#ifndef __ZW_LOGGING__H__
#define __ZW_LOGGING__H__

#include "zw_common.h"

extern ZWAppConfig gConfig;
extern void (*gPublishLogsEmit)(const char* fmt, ...);

#define dprint(fmt, ...) do { \
    if (gConfig.debug) { \
        Serial.printf("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
    } } while (0)

#define zlog(fmt, ...) do { \
    if (gConfig.publishLogs && gPublishLogsEmit) { \
        gPublishLogsEmit(fmt, ##__VA_ARGS__); \
    } else { \
        Serial.printf(fmt, ##__VA_ARGS__); \
    } } while (0)

#endif
