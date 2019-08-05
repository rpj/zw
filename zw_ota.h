#ifndef __ZW_OTA__H__
#define __ZW_OTA__H__

#include "zw_provision.h"

#define OTA_RESET_DELAY 5
#define OTA_UPDATE_PRCNT_REPORT 10

bool runUpdate(
    const char *url, 
    const char *md5, 
    size_t sizeInBytes,
    void (*preUpdateIRQDisable)(),
    bool (*completedCallback)());

#endif