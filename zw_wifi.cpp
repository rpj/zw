#include <WiFi.h>
#include "zw_logging.h"
#include "zw_displays.h"
#include "zw_provision.h"

extern DisplaySpec *gDisplays;

bool zwWiFiInit(const char *hostname, ZWAppConfig config)
{
    dprint("Disabling WiFi AP\n");
    WiFi.mode(WIFI_MODE_STA);
    WiFi.enableAP(false);

    if (!WiFi.setHostname(hostname))
    {
        dprint("WARNING: failed to set hostname\n");
    }

    auto bstat = WiFi.begin(EEPROMCFG_WiFiSSID, EEPROMCFG_WiFiPass);
    dprint("Connecting to to '%s'...\n", EEPROMCFG_WiFiSSID);
    dprint("WiFi.begin() -> %d\n", bstat);

    // TODO: timeout!
    uint8_t _wakeup[][4] = {
        {1, 0, 0, 0},
        {1, 1, 0, 0},
        {1, 1, 1, 0},
        {1, 1, 1, 1},
        {1, 1, 1, 3},
        {1, 1, 1, 7},
        {1, 1, 1, 15},
        {1, 1, 9, 15},
        {1, 9, 9, 15},
        {9, 9, 9, 15},
        {25, 9, 9, 15},
        {57, 9, 9, 15},
        {57, 9, 9, 15},
        {56, 9, 9, 15},
        {56, 8, 9, 15},
        {56, 8, 8, 15},
        {56, 8, 8, 14},
        {56, 8, 8, 12},
        {56, 8, 8, 8},
        {56, 8, 8, 0},
        {56, 8, 0, 0},
        {56, 0, 0, 0},
        {48, 0, 0, 0},
        {32, 0, 0, 0},
        {0, 0, 0, 0},
    };
    auto _wakeup_size = sizeof(_wakeup) / sizeof(_wakeup[0]);
    int _lc = 0;
    while (WiFi.status() != WL_CONNECTED)
    {
        gDisplays[0].disp->setSegments(_wakeup[_lc++ % _wakeup_size]);
    }

    zlog("WiFi adapter %s connected to '%s' as %s\n", WiFi.macAddress().c_str(),
         EEPROMCFG_WiFiSSID, WiFi.localIP().toString().c_str());

    return true;
}
