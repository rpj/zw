#include "zw_provision.h"
#include "zw_logging.h"
#include "zw_wifi.h"
#include "zw_redis.h"

#include <WiFi.h>

char *EEPROMCFG_WiFiSSID = NULL;
char *EEPROMCFG_WiFiPass = NULL;
char *EEPROMCFG_RedisHost = NULL;
char *EEPROMCFG_RedisPass = NULL;
char *EEPROMCFG_OTAHost = NULL;
uint16_t EEPROMCFG_RedisPort = 0;
String gHostname;
char __hnShadowBuf[ZW_EEPROM_SIZE];

void __debugClearEEPROM()
{
    dprint("__debugClearEEPROM()\n");
    // not the quickest, but the most memory-efficient, algorithm
    for (int i = 0; i < EEPROM_SIZE; i++)
        EEPROM.write(i, 0);
    EEPROM.commit();
}

void EEPROM_setup()
{
    EEPROM.begin(EEPROM_SIZE);
}

#define CFG_ELEMENTS 6
#define CFG_HEADER_SIZE (CFG_ELEMENTS * sizeof(uint16_t))

void CFG_EEPROM_read()
{
    uint16_t lengths[CFG_ELEMENTS];
    dprint("CFG_EEPROM_read: reading %d for lengths\n", CFG_HEADER_SIZE);
    zwassert(EEPROM.readBytes(CFG_EEPROM_ADDR, lengths, CFG_HEADER_SIZE) == CFG_HEADER_SIZE);

    String lengthsStr = "";
    for (int i = 0; i < CFG_ELEMENTS; i++)
    {
        lengthsStr += String(lengths[i]) + " ";
    }
    dprint("CFG_EEPROM_read: LENGTHS: %s\n", lengthsStr.c_str());

    EEPROMCFG_WiFiSSID = (char *)malloc(lengths[0] + 1);
    bzero(EEPROMCFG_WiFiSSID, lengths[0] + 1);
    auto offset = CFG_EEPROM_ADDR + CFG_HEADER_SIZE;
    zwassert(EEPROM.readBytes(offset, EEPROMCFG_WiFiSSID, lengths[0]) == lengths[0]);
    offset += lengths[0];
    dprint("CFG_EEPROM_read: WIFI SSID: %s\n", EEPROMCFG_WiFiSSID);

    EEPROMCFG_WiFiPass = (char *)malloc(lengths[1] + 1);
    bzero(EEPROMCFG_WiFiPass, lengths[1] + 1);
    zwassert(EEPROM.readBytes(offset, EEPROMCFG_WiFiPass, lengths[1]) == lengths[1]);
    offset += lengths[1];
    dprint("CFG_EEPROM_read: WIFI PASS: %s\n", EEPROMCFG_WiFiPass);

    EEPROMCFG_RedisHost = (char *)malloc(lengths[2] + 1);
    bzero(EEPROMCFG_RedisHost, lengths[2] + 1);
    zwassert(EEPROM.readBytes(offset, EEPROMCFG_RedisHost, lengths[2]) == lengths[2]);
    offset += lengths[2];
    dprint("CFG_EEPROM_read: REDIS IP: %s\n", EEPROMCFG_RedisHost);

    zwassert(EEPROM.readBytes(offset, &EEPROMCFG_RedisPort, lengths[3]) == lengths[3]);
    offset += lengths[3];
    dprint("CFG_EEPROM_read: REDIS PORT: %d\n", EEPROMCFG_RedisPort);

    EEPROMCFG_RedisPass = (char *)malloc(lengths[4] + 1);
    bzero(EEPROMCFG_RedisPass, lengths[4] + 1);
    zwassert(EEPROM.readBytes(offset, EEPROMCFG_RedisPass, lengths[4]) == lengths[4]);
    offset += lengths[4];
    dprint("CFG_EEPROM_read: REDIS PASS: %s\n", EEPROMCFG_RedisPass);

    EEPROMCFG_OTAHost = (char *)malloc(lengths[5] + 1);
    bzero(EEPROMCFG_OTAHost, lengths[5] + 1);
    zwassert(EEPROM.readBytes(offset, EEPROMCFG_OTAHost, lengths[5]) == lengths[5]);
    offset += lengths[5];
    dprint("CFG_EEPROM_read: OTA HOST: %s\n", EEPROMCFG_OTAHost);
}

#define PSTRING_LENGTH_LIMIT (CFG_EEPROM_SIZE / 2)

inline bool __provStrCheck(const char *str)
{
    return str && strnlen(str, CFG_EEPROM_SIZE - CFG_HEADER_SIZE) <= PSTRING_LENGTH_LIMIT;
}

bool checkUnitProvisioning()
{
    auto eeHnLength = EEPROM.readBytes(ZW_EEPROM_HOSTNAME_ADDR, __hnShadowBuf, ZW_EEPROM_SIZE);

    if (!eeHnLength || eeHnLength > ZW_EEPROM_SIZE)
        return false;

    gHostname = String(__hnShadowBuf);

    // it's possible for eeHnLength to be valid while the hostname itself
    // contains garbade data, in which case gHostname may have length
    // larger than the allowed size
    if (gHostname.length() > ZW_EEPROM_SIZE)
        return false;

    dprint("EEPROM HOSTNAME (%db, %db) %s\n", eeHnLength, gHostname.length(), gHostname.c_str());

    CFG_EEPROM_read();

    return __provStrCheck(EEPROMCFG_WiFiSSID) && __provStrCheck(EEPROMCFG_WiFiPass) &&
           __provStrCheck(EEPROMCFG_RedisHost) && __provStrCheck(EEPROMCFG_RedisPass) &&
           __provStrCheck(EEPROMCFG_OTAHost) && EEPROMCFG_RedisPort > 0;
}

#if ZEROWATCH_PROVISIONING_MODE
void provisionUnit()
{
    dprint("***** ZeroWatch provisioning mode *****\n");
    dprint("\tHostname:       \t%s\n", ZWPROV_HOSTNAME);
    dprint("\tWiFi SSID:      \t%s\n", ZWPROV_WIFI_SSID);
    dprint("\tWiFi password:  \t%s\n", ZWPROV_WIFI_PASSWORD);
    dprint("\tRedis host:     \t%s\n", ZWPROV_REDIS_HOST);
    dprint("\tRedis password: \t%s\n", ZWPROV_REDIS_PASSWORD);
    dprint("\tRedis port:     \t%u\n", ZWPROV_REDIS_PORT);
    dprint("\tOTA host:       \t%s\n", ZWPROV_OTA_HOST);
    dprint("***** WILL COMMIT THESE VALUES TO EEPROM IN %d SECONDS.... *****\n", ZWPROV_MODE_WRITE_DELAY);
    delay(ZWPROV_MODE_WRITE_DELAY * 1000);
    dprint("***** COMMITTING ABOVE VALUES TO EEPROM: *****\n");

    auto hostname = String(ZWPROV_HOSTNAME);
    zwassert(EEPROM.writeString(ZW_EEPROM_HOSTNAME_ADDR, hostname) == hostname.length());

    uint16_t lengths[CFG_ELEMENTS];

    auto _local_port = ZWPROV_REDIS_PORT;
    lengths[0] = strlen(ZWPROV_WIFI_SSID);
    lengths[1] = strlen(ZWPROV_WIFI_PASSWORD);
    lengths[2] = strlen(ZWPROV_REDIS_HOST);
    lengths[3] = sizeof(_local_port); // redis port, always 16 bits
    lengths[4] = strlen(ZWPROV_REDIS_PASSWORD);
    lengths[5] = strlen(ZWPROV_OTA_HOST);

    dprint("*** ZeroWatch provisioning: lengths: ");
    for (int i = 0; i < CFG_ELEMENTS; i++)
    {
        dprint("%d ", lengths[i]);
    }
    dprint("\n");

    auto writeLen = EEPROM.writeBytes(CFG_EEPROM_ADDR, lengths, CFG_HEADER_SIZE);
    if (writeLen != CFG_HEADER_SIZE)
    {
        dprint("*** ZeroWatch provisioning ERROR: Config write (%d) failed\n", writeLen);
        return;
    }

    auto offset = CFG_EEPROM_ADDR + CFG_HEADER_SIZE;

    void **ptrptrs = (void **)malloc(sizeof(void *) * CFG_ELEMENTS);
    ptrptrs[0] = (void *)ZWPROV_WIFI_SSID;
    ptrptrs[1] = (void *)ZWPROV_WIFI_PASSWORD;
    ptrptrs[2] = (void *)ZWPROV_REDIS_HOST;
    ptrptrs[3] = (void *)&_local_port;
    ptrptrs[4] = (void *)ZWPROV_REDIS_PASSWORD;
    ptrptrs[5] = (void *)ZWPROV_OTA_HOST;

    for (int i = 0; i < CFG_ELEMENTS; i++)
    {
        dprint("*** ZeroWatch provisioning: Writing %d bytes of element %d to address %d\n",
               lengths[i], i, offset);
        writeLen = EEPROM.writeBytes(offset, ptrptrs[i], lengths[i]);
        if (writeLen != lengths[i])
        {
            dprint("*** ZeroWatch provisioning ERROR: element %d failed to write (%d != %d)\n", i, writeLen, lengths[i]);
            return;
        }
        offset += lengths[i];
    }

    if (EEPROM.commit())
    {
        dprint("*** ZeroWatch provisioning: Write complete, verifying data...\n");

        if (checkUnitProvisioning())
        {
            auto hnVerify = EEPROM.readString(ZW_EEPROM_HOSTNAME_ADDR);
            zwassert(!memcmp(ZWPROV_HOSTNAME, hnVerify.c_str(), strlen(ZWPROV_HOSTNAME)));
            zwassert(!memcmp(ptrptrs[0], EEPROMCFG_WiFiSSID, lengths[0]));
            zwassert(!memcmp(ptrptrs[1], EEPROMCFG_WiFiPass, lengths[1]));
            zwassert(!memcmp(ptrptrs[2], EEPROMCFG_RedisHost, lengths[2]));
            zwassert(!memcmp(ptrptrs[3], &EEPROMCFG_RedisPort, lengths[3]));
            zwassert(!memcmp(ptrptrs[4], EEPROMCFG_RedisPass, lengths[4]));
            zwassert(!memcmp(ptrptrs[5], EEPROMCFG_OTAHost, lengths[5]));

            dprint("*** ZeroWatch provisioning: data verified correctly!\n");

#if ZEROWATCH_PROVISIONING_OPTION_FUNCTIONAL_VERIFICATION
            dprint("*** ZeroWatch provisioning: starting functional check...\n");

            ZWAppConfig blankConfig;
            if (!zwWiFiInit(hnVerify.c_str(), blankConfig))
            {
                dprint("*** ZeroWatch provisioning: WiFi check failed\n");
                __haltOrCatchFire();
            }

            ZWRedisHostConfig redisConfig = {
                .host = EEPROMCFG_RedisHost,
                .port = EEPROMCFG_RedisPort,
                .password = EEPROMCFG_RedisPass};
            ZWRedis redisCheck(hnVerify, redisConfig);

            if (!redisCheck.connect())
            {
                dprint("*** ZeroWatch provisioning: Redis check failed\n");
                __haltOrCatchFire();
            }

            if (ZWPROV_FUNCTIONAL_VERIFICATION_REGISTRY_DEVICE_IN)
            {
                dprint("*** ZeroWatch provisioning: registering device in '%s'...\n",
                       ZWPROV_FUNCTIONAL_VERIFICATION_REGISTRY_DEVICE_IN);

                if (!redisCheck.registerDevice(ZWPROV_FUNCTIONAL_VERIFICATION_REGISTRY_DEVICE_IN,
                                               ZWPROV_HOSTNAME, WiFi.macAddress().c_str()))
                {
                    dprint("*** ZeroWatch provisioning WARNING: registration failed (or already registered)\n");
                }
            }

#else
            dprint("*** ZeroWatch provisioning WARNING: functional verification will be SKIPPED\n");
#endif

            dprint("\n\n***** ZeroWatch provisioning completed successfully!!\n");
        }
        else
        {
            dprint("*** ZeroWatch provisioning FAILED! :shrug:\n");
        }
    }
    else
    {
        dprint("*** ZeroWatch provisioning: commit failed\n");
    }

    __haltOrCatchFire();
}
#endif

void verifyProvisioning()
{
    EEPROM_setup();

#if ZEROWATCH_DEL_PROVISIONS
    dprint("***** ZEROWATCH_DEL_PROVISIONS is set! Waiting %d seconds... *****\n", ZWPROV_MODE_WRITE_DELAY);
    delay(ZWPROV_MODE_WRITE_DELAY * 1000);
    dprint("***** CLEARING PROVISIONING!! ***** \n");
    __debugClearEEPROM();
#endif

#if ZEROWATCH_PROVISIONING_MODE
    provisionUnit();
#endif

    if (!checkUnitProvisioning())
    {
        zlog("\n\nThis device is not provisioned! Please use ZEROWATCH_PROVISIONING_MODE to initialize it.");
        __haltOrCatchFire();
    }
}