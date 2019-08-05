// zero_watch.ino
// (C) 2019 Ryan Joseph <ryan@electricsheep.co>
// https://github.com/rpj/zw

#include <WiFi.h>
#include <Update.h>
#include <ArduinoJson.h>

#include "zw_common.h"
#include "zw_logging.h"
#include "zw_redis.h"
#include "zw_provision.h"
#include "zw_displays.h"
#include "zw_otp.h"
#include "zw_ota.h"

#define CONTROL_POINT_SEP_CHAR '#'
#define SER_BAUD 115200
#define DEF_BRIGHT 0
#define HB_CHECKIN 5
#if DEBUG
#define DEF_REFRESH 20
#else
#define DEF_REFRESH 240
#endif

ZWAppConfig gConfig = {
    .brightness = DEF_BRIGHT,
    .refresh = DEF_REFRESH,
    .debug = DEBUG,
    .publishLogs = false,
    .pauseRefresh = false};
ZWRedis *gRedis = NULL;
DisplaySpec *gDisplays = NULL;
void (*gPublishLogsEmit)(const char *fmt, ...);
unsigned long long gSecondsSinceBoot = 0;
unsigned long long gLastRefreshTick = 0;
int _last_free = 0;
unsigned long gUDRA = 0;
unsigned long immediateLatency = 0;
StaticJsonBuffer<1024> jsonBuf;
hw_timer_t *__isrTimer = NULL;
portMUX_TYPE __isrMutex = portMUX_INITIALIZER_UNLOCKED;
volatile unsigned long __isrCount = 0;

bool wifi_init()
{
    dprint("Disabling WiFi AP\n");
    WiFi.mode(WIFI_MODE_STA);
    WiFi.enableAP(false);

    auto bstat = WiFi.begin(EEPROMCFG_WiFiSSID, EEPROMCFG_WiFiPass);
    dprint("Connecting to to '%s'...\n", EEPROMCFG_WiFiSSID);
    dprint("WiFi.begin() -> %d\n", bstat);

    runAnimation(gDisplays[0].disp, "light_loop", true);
    gDisplays[0].disp->clear();
    gDisplays[0].disp->showNumberHexEx(0xffff, 64, true);
    // TODO: timeout!
    int _c = 0;
    while (WiFi.status() != WL_CONNECTED)
    {
        gDisplays[0].disp->showNumberHexEx((++_c << 8) + 0xff, 64, true);
    }

    zlog("WiFi adapter %s connected to '%s' as %s\n", WiFi.macAddress().c_str(),
         EEPROMCFG_WiFiSSID, WiFi.localIP().toString().c_str());
    gDisplays[0].disp->showNumberHexEx(0xFF00 | WiFi.status(), 64, false);

    return true;
}

bool processGetValue(String &imEmit, ZWRedisResponder &responder)
{
    bool matched = true;
    imEmit.toLowerCase();

    if (imEmit.startsWith("ip") || imEmit.endsWith("address"))
    {
        responder.setValue("%s", WiFi.localIP().toString().c_str());
    }
    else if (imEmit.startsWith("mem"))
    {
        auto cur_free = ESP.getFreeHeap();
        responder.setValue(
            "{ \"current\": %d, \"last\": %d, \"delta\": %d, \"heap\": %d }",
            cur_free, _last_free, cur_free - _last_free, ESP.getHeapSize());
    }
    else if (imEmit.startsWith("up"))
    {
        responder.setExpire(5);
        responder.setValue("%s", String(millis() / 1000).c_str());
    }
    else if (imEmit.startsWith("ver"))
    {
        responder.setValue(
            "{ \"version\": \"%s\", \"sketchMD5\": \"%s\", "
            "\"sketchSize\": %d, \"sdk\": \"%s\", \"chipRev\": %d}",
            ZEROWATCH_VER, ESP.getSketchMD5().c_str(), ESP.getSketchSize(),
            ESP.getSdkVersion(), ESP.getChipRevision());
    }
    else if (imEmit.equals("demo"))
    {
        demoMode(gDisplays);
    }
    else if (imEmit.equals("latency"))
    {
        responder.setValue("{ \"immediate\": %d, \"rollingAvg\": %d }", 
            immediateLatency, gUDRA);
    }
    else
    {
        matched = false;
    }

    return matched;
}

bool ctrlPoint_reset()
{
    dprint("[CMD] RESETING!\n");
    zlog("[CMD] RESETING!");
    gRedis->clearControlPoint();
    ESP.restart();
    return true; // never reached
}

bool ctrlPoint_clearbootcount()
{
    dprint("[CMD] Clearing boot count!\n");
    zlog("[CMD] Clearing boot count!\n");
    return gRedis->incrementBootcount(true) == 0;
}

bool processControlPoint(String &imEmit, ZWRedisResponder &responder)
{
    auto sepIdx = imEmit.indexOf(CONTROL_POINT_SEP_CHAR);

    if (sepIdx == -1 && sepIdx < imEmit.length())
    {
        zlog("ERROR: control point write is malformed ('%s')\n", imEmit.c_str());
        return false;
    }

    auto cmd = imEmit.substring(0, sepIdx);
    auto otp = (uint16_t)imEmit.substring(sepIdx + 1).toInt();

    zlog("Control point has '%s' with OTP %d...\n", cmd.c_str(), otp);

    if (!otpCheck(otp))
    {
        zlog("ERROR: processControlPoint: Not authorized!\n");
        return false;
    }

    zlog("Control point is authorized.\n");
    bool (*ctrlPointFunc)() = NULL;

    if (cmd.equals("reset"))
    {
        ctrlPointFunc = ctrlPoint_reset;
    }
    else if (cmd.equals("clearbootcount"))
    {
        ctrlPointFunc = ctrlPoint_clearbootcount;
    }

    if (ctrlPointFunc)
    {
        zlog("Control point command '%s' is valid: executing.\n", cmd.c_str());
        return ctrlPointFunc();
    }
    else
    {
        zlog("WARNING: unknown control point command '%s'\n", cmd.c_str());
    }

    return true;
}

void preUpdateIRQDisableFunc()
{
    zlog("preUpdateIRQDisableFunc disabling __isrTimer\n");
    timerEnd(__isrTimer);
}

bool processUpdate(String &updateJson, ZWRedisResponder &responder)
{
    JsonObject &updateObj = jsonBuf.parseObject(updateJson.c_str());

    if (updateObj.success())
    {
        auto url = updateObj.get<char *>("url");
        auto md5 = updateObj.get<char *>("md5");
        auto szb = updateObj.get<int>("size");
        auto otp = updateObj.get<unsigned long>("otp");

        if (!otpCheck(otp))
        {
            zlog("ERROR: Not authorized\n");
            return false;
        }

        zlog("Accepted OTA OTP '%ld'\n", otp);

        if (url && md5 && szb > 0)
        {
            auto fqUrlLen = strlen(EEPROMCFG_OTAHost) + strlen(url) + 2;
            char *fqUrl = (char *)malloc(fqUrlLen);
            bzero(fqUrl, fqUrlLen);

            auto fqWrote = snprintf(fqUrl, fqUrlLen, "%s/%s", EEPROMCFG_OTAHost, url);
            if (fqWrote != fqUrlLen - 1)
                zlog("WARNING: wrote %d of expected %d bytes for fqUrl\n", fqWrote, fqUrlLen);

            zlog("Starting OTA update of %0.2fKB\n", (szb / 1024.0));
            zlog("Image source (md5=%s):\n\t%s\n", md5, fqUrl);

            if (runUpdate(fqUrl, md5, szb, preUpdateIRQDisableFunc,
                []() { return gRedis->postCompletedUpdate(); }))
            {
                zlog("OTA update wrote successfully! Restarting in %d seconds...\n", 
                    OTA_RESET_DELAY);
                delay(OTA_RESET_DELAY * 1000);
                ESP.restart();
                return true; // never reached
            }
            else
            {
                zlog("ERROR: OTA failed! %d\n", Update.getError());
            }
        }
        else
        {
            zlog("ERROR: Bad OTA params (%p, %p, %d)\n", url, md5, szb);
        }
    }
    else
    {
        zlog("ERROR: failed to parse update\n");
    }

    return false;
}

void redis_publish_logs_emit(const char *fmt, ...)
{
    // this function should never be called before gRedis is valid
    zwassert(gRedis != NULL);

    char buf[1024];
    bzero(buf, 1024);
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    auto len = strlen(buf);

    if (buf[len - 1] == '\n')
        buf[len - 1] = '\0';

    gRedis->publishLog(buf);
    va_end(args);
}

void readConfigAndUserKeys()
{
    auto curCfg = gRedis->readConfig();

    if (curCfg.brightness >= 0 && curCfg.brightness < 8 &&
        curCfg.brightness != gConfig.brightness)
    {
        zlog("Read new brightness level: %d\n", curCfg.brightness);
        gConfig.brightness = curCfg.brightness;
        EXEC_ALL_DISPS(gDisplays, setBrightness(gConfig.brightness));
    }

    if (curCfg.refresh >= 5 && curCfg.refresh != gConfig.refresh)
    {
        zlog("Read new refresh rate: %d\n", curCfg.refresh);
        gConfig.refresh = curCfg.refresh;
    }

    if (curCfg.debug != gConfig.debug)
    {
        zlog("Read new debug setting: %sabled\n", curCfg.debug ? "en" : "dis");
        gConfig.debug = curCfg.debug;
    }

    if (curCfg.publishLogs != gConfig.publishLogs)
    {
        zlog("Read new publishLogs setting: %sabled\n", curCfg.publishLogs ? "en" : "dis");
        gConfig.publishLogs = curCfg.publishLogs;
    }

    if (curCfg.pauseRefresh != gConfig.pauseRefresh)
    {
        gConfig.pauseRefresh = curCfg.pauseRefresh;
        zlog("REFRESH %s\n", gConfig.pauseRefresh ? "PAUSED" : "RESUMED");
        dprint("REFRESH %s\n", gConfig.pauseRefresh ? "PAUSED" : "RESUMED");
    }

    if (!gConfig.pauseRefresh)
    {
        gRedis->handleUserKey(":config:getValue", processGetValue);
        gRedis->handleUserKey(":config:controlPoint", processControlPoint);
        gRedis->handleUserKey(":config:update", processUpdate);
    }
}

void heartbeat()
{
    static uint64_t __hb_count = 0;
    if (gRedis)
    {
        if (!gRedis->heartbeat(gConfig.refresh * 5))
        {
            zlog("WARNING: heartbeat failed!\n");
        }

        if ((__hb_count++ % HB_CHECKIN))
        {
            gRedis->checkin(gSecondsSinceBoot, WiFi.localIP().toString().c_str(), 
                immediateLatency, gUDRA, gConfig.refresh * 5);
        }
    }
}

void tick(bool forceUpdate = false)
{
    if (gConfig.pauseRefresh)
        if (!forceUpdate)
            return;

    zlog("Awake at us=%lu tick=%lld\n", micros(), gSecondsSinceBoot);

    for (DisplaySpec *w = gDisplays; w->clockPin != -1 && w->dioPin != -1; w++)
        updateDisplay(w);

    _last_free = ESP.getFreeHeap();
}

void __isr()
{
    portENTER_CRITICAL(&__isrMutex);
    ++__isrCount;
    portEXIT_CRITICAL(&__isrMutex);
}

void loop()
{
    if (__isrCount) {
        portENTER_CRITICAL(&__isrMutex);
        --__isrCount;
        portEXIT_CRITICAL(&__isrMutex);
        ++gSecondsSinceBoot;
        dprint("%c%s", !(gSecondsSinceBoot % 5) ? '|' : '.', gSecondsSinceBoot % gConfig.refresh ? "" : "\n");
    }
    
    if (!(gSecondsSinceBoot % gConfig.refresh) && gLastRefreshTick != gSecondsSinceBoot)
    {
        gLastRefreshTick = gSecondsSinceBoot;
        readConfigAndUserKeys();
        heartbeat();
        tick();
    }
}

void setup()
{
    pinMode(LED_BLTIN, OUTPUT);
    Serial.begin(SER_BAUD);
    
    verifyProvisioning();

    zlog("\n%s v" ZEROWATCH_VER " starting...\n", gHostname.c_str());

    gDisplays = zwdisplayInit(gHostname);
    if (gDisplays && wifi_init())
    {
        auto verNum = String(ZEROWATCH_VER);
        verNum.replace(".", "");
        gDisplays[0].disp->showNumberDec(verNum.toInt(), true);

        delay(2000);

        ZWRedisHostConfig redisConfig = {
            .host = EEPROMCFG_RedisHost,
            .port = EEPROMCFG_RedisPort,
            .password = EEPROMCFG_RedisPass};

        gRedis = new ZWRedis(gHostname, redisConfig);

        if (gRedis->connect())
        {
            zlog("Redis connection established, reading config...\n");
            
            readConfigAndUserKeys();

            zlog("Fully initialized! (debug %sabled)\n", gConfig.debug ? "en" : "dis");

            if (gConfig.debug)
                delay(5000);

            gPublishLogsEmit = redis_publish_logs_emit;

            __isrTimer = timerBegin(0, 80, true);
            timerAttachInterrupt(__isrTimer, &__isr, true);
            timerAlarmWrite(__isrTimer, 1000000, true);
            timerAlarmEnable(__isrTimer);

            zlog("Boot count: %d\n", gRedis->incrementBootcount());
            zlog("%s v" ZEROWATCH_VER " up & running\n", gHostname.c_str());

            tick(true);
        }
        else
        {
            zlog("ERROR: redis init failed!");
        }
    }
}
