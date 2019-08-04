#ifndef __ZW_PROVISION__H__
#define __ZW_PROVISION__H__

#include <EEPROM.h>

// provisioning definitions
#define ZEROWATCH_PROVISIONING_MODE 0
#if ZEROWATCH_PROVISIONING_MODE
// these will be written to EEPROM
#define ZWPROV_HOSTNAME ""
#define ZWPROV_WIFI_SSID ""
#define ZWPROV_WIFI_PASSWORD ""
#define ZWPROV_REDIS_HOST ""
#define ZWPROV_REDIS_PASSWORD ""
#define ZWPROV_REDIS_PORT 6379
#define ZWPROV_OTA_HOST ""
#endif

#define ZW_EEPROM_SIZE 32
#define ZW_EEPROM_HOSTNAME_ADDR 0
#define CFG_EEPROM_SIZE 3192
#define CFG_EEPROM_ADDR ZW_EEPROM_SIZE
#define EEPROM_SIZE ZW_EEPROM_SIZE + CFG_EEPROM_SIZE
#define ZWPROV_MODE_WRITE_DELAY 60

// dangerous!
#define ZEROWATCH_DEL_PROVISIONS 0

extern char *EEPROMCFG_WiFiSSID;
extern char *EEPROMCFG_WiFiPass;
extern char *EEPROMCFG_RedisHost;
extern char *EEPROMCFG_RedisPass;
extern char *EEPROMCFG_OTAHost;
extern uint16_t EEPROMCFG_RedisPort;

extern String gHostname;

void verifyProvisioning();

#endif