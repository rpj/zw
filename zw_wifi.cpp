#include <WiFi.h>
#include "zw_logging.h"
#include "zw_displays.h"
#include "zw_provision.h"

bool zwWiFiInit(const char *hostname, ZWAppConfig config)
{
    WiFi.mode(WIFI_MODE_STA);
    WiFi.enableAP(false);

    if (!WiFi.setHostname(hostname))
    {
        dprint("WARNING: failed to set hostname\n");
    }

    auto bstat = WiFi.begin(EEPROMCFG_WiFiSSID, EEPROMCFG_WiFiPass);
    zlog("Connecting to '%s'\n", EEPROMCFG_WiFiSSID);

    // TODO: timeout!
    while (WiFi.status() != WL_CONNECTED) {}

    zlog("Connected as %s\n", WiFi.localIP().toString().c_str());

    return true;
}
