#include <WiFi.h>
#include "zw_logging.h"
#include "zw_displays.h"
#include "zw_provision.h"

bool zwWiFiInit(const char *hostname, ZWAppConfig config)
{
    zlog("Disabling WiFi AP\n");
    WiFi.mode(WIFI_MODE_STA);
    WiFi.enableAP(false);

    if (!WiFi.setHostname(hostname))
    {
        dprint("WARNING: failed to set hostname\n");
    }

    auto bstat = WiFi.begin(EEPROMCFG_WiFiSSID, EEPROMCFG_WiFiPass);
    zlog("Connecting to to '%s'...\n", EEPROMCFG_WiFiSSID);
    zlog("WiFi.begin() -> %d\n", bstat);

    // TODO: timeout!
    while (WiFi.status() != WL_CONNECTED) {}

    zlog("WiFi adapter %s connected to '%s' as %s\n", WiFi.macAddress().c_str(),
         EEPROMCFG_WiFiSSID, WiFi.localIP().toString().c_str());

    return true;
}
