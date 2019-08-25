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
#include "zw_wifi.h"

#define DEEP_SLEEP_MODE_ENABLE 1

#define CONTROL_POINT_SEP_CHAR '#'
#define SER_BAUD 115200
#define DEF_BRIGHT 0
#define CHECKIN_EVERY_X_REFRESH 5
#define CHECKIN_EXPIRY_MULT 2
#define HEARTBEAT_EXPIRY_MULT 5
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
    .pauseRefresh = false,
    .deepSleepMode = DEEP_SLEEP_MODE_ENABLE
};
ZWRedis *gRedis = NULL;
DisplaySpec *gDisplays = NULL;
void (*gPublishLogsEmit)(const char *fmt, ...);
unsigned long gBootCount = 0;
unsigned long long gSecondsSinceBoot = 0;
unsigned long long gLastRefreshTick = 0;
int _last_free = 0;
unsigned long gUDRA = 0;
unsigned long immediateLatency = 0;
StaticJsonBuffer<1024> jsonBuf;
hw_timer_t *__isrTimer = NULL;
portMUX_TYPE __isrMutex = portMUX_INITIALIZER_UNLOCKED;
volatile unsigned long __isrCount = 0;

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

            if (runUpdate(fqUrl, md5, szb, preUpdateIRQDisableFunc, []() {
                    if (gRedis->incrementBootcount(true) != 0)
                        zlog("WARNING: unable to reset bootcount!\n");
                    return gRedis->postCompletedUpdate(); 
                }))
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

            free(fqUrl);
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

bool processDisplaysConfig(String &updateJson, ZWRedisResponder &responder)
{
    dprint("GOT DISPLAYS CONFIG: %s\n", updateJson.c_str());
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
    va_end(args);

    auto len = strlen(buf);

    if (buf[len - 1] == '\n')
        buf[len - 1] = '\0';

#define PUB_FMT_STR "{\"source\":\"%s\",\"type\":\"VALUE\",\"ts\":%s,\"value\":{\"logline\":\"%s\"}}"
    auto tickStr = String((unsigned long)gSecondsSinceBoot);
    auto pubLen = strlen(PUB_FMT_STR) + len + gHostname.length() + tickStr.length();
    char *jbuf = (char*)malloc(pubLen);
    bzero(jbuf, pubLen);
    snprintf(jbuf, pubLen, PUB_FMT_STR, gHostname.c_str(), tickStr.c_str(), buf);
    gRedis->publishLog(jbuf);
    free(jbuf);
}

void readAndSetTime()
{
    RTC_TimeTypeDef ts;
    bzero(&ts, sizeof(ts));
    gRedis->getTime(&ts.Hours, &ts.Minutes, &ts.Seconds);
    if (!(!ts.Hours && !ts.Minutes && !ts.Seconds)) {
        M5.Rtc.SetTime(&ts);
        zlog("Set time (from Redis) to %02d:%02d:%02d\n", 
        ts.Hours, ts.Minutes, ts.Seconds);
    }
    else {
        zlog("Failed to get time from Redis (or it is exactly midnight!)\n");
    }
}

void readConfigAndUserKeys()
{
    auto curCfg = gRedis->readConfig();
    bool dirty = false;

#define UPDATE_IF_CHANGED(field)                           \
    if (curCfg.field != gConfig.field)                     \
    {                                                      \
        zlog("[Config] %s -> %d\n", #field, curCfg.field); \
        gConfig.field = curCfg.field;                      \
    }

#define UPDATE_IF_CHANGED_ELSE_MARKED_DIRTY_WITH_EXTRAEXTRA(field, extraCond, extraIfUpdated) \
    if ((extraCond) && curCfg.field != gConfig.field)                                         \
    {                                                                                         \
        zlog("[Config] %s -> %d\n", #field, curCfg.field);                                    \
        gConfig.field = curCfg.field;                                                         \
        extraIfUpdated;                                                                       \
    }                                                                                         \
    else if (!(extraCond))                                                                    \
    {                                                                                         \
        zlog("Redis has invalid %s configuration, %d: correcting to %d\n",                    \
             #field, curCfg.field, gConfig.field);                                            \
        curCfg.field = gConfig.field;                                                         \
        dirty = true;                                                                         \
    }

#define UPDATE_IF_CHANGED_ELSE_MARKED_DIRTY_WITH_EXTRA(field, extraCond) \
    UPDATE_IF_CHANGED_ELSE_MARKED_DIRTY_WITH_EXTRAEXTRA(field, extraCond, do {} while (0))

#if M5STACKC
    UPDATE_IF_CHANGED_ELSE_MARKED_DIRTY_WITH_EXTRAEXTRA(brightness,
                                                        curCfg.brightness >= 0 && curCfg.brightness < 8,
                                                        M5.Axp.ScreenBreath(gConfig.brightness + 7));
#else
    UPDATE_IF_CHANGED_ELSE_MARKED_DIRTY_WITH_EXTRAEXTRA(brightness,
                                                        curCfg.brightness >= 0 && curCfg.brightness < 8,
                                                        EXEC_ALL_DISPS(gDisplays, setBrightness(gConfig.brightness)));
#endif

    UPDATE_IF_CHANGED_ELSE_MARKED_DIRTY_WITH_EXTRA(refresh, curCfg.refresh >= 5);

    UPDATE_IF_CHANGED(debug);

    UPDATE_IF_CHANGED(publishLogs);

    UPDATE_IF_CHANGED(pauseRefresh);

    UPDATE_IF_CHANGED(deepSleepMode);

    if (dirty)
    {
        auto badCount = gRedis->updateConfig(curCfg);
        if (badCount)
        {
            zlog("WARNING: tried to update Redis config but hit %d errors\n", badCount);
        }
    }

    if (!gConfig.pauseRefresh)
    {
        gRedis->handleUserKey(":config:getValue", processGetValue);
        gRedis->handleUserKey(":config:controlPoint", processControlPoint);
        gRedis->handleUserKey(":config:update", processUpdate);
        gRedis->handleUserKey(":config:displays", processDisplaysConfig);
    }
}

void heartbeat()
{
    static uint64_t __hb_count = 0;
    if (gRedis)
    {
        if (!gRedis->heartbeat(gConfig.refresh * HEARTBEAT_EXPIRY_MULT))
        {
            zlog("WARNING: heartbeat failed!\n");
        }

        if (gConfig.deepSleepMode || (__hb_count++ % CHECKIN_EVERY_X_REFRESH))
        {
            gRedis->checkin(gConfig.deepSleepMode ? gBootCount : gSecondsSinceBoot, WiFi.localIP().toString().c_str(),
                            immediateLatency, gUDRA, gConfig.refresh * CHECKIN_EVERY_X_REFRESH * CHECKIN_EXPIRY_MULT);
        }
    }
}

#define R_GREY 0xc618
#define R_DGREY 0x7bef
#if M5STACKC
void zwM5StickC_UpdateBatteryDisplay()
{
    double vbat = 0.0;
    int discharge,charge;
    double temp = 0.0;

    vbat = M5.Axp.GetVbatData() * 1.1 / 1000;
    charge = M5.Axp.GetIchargeData() / 2;
    discharge = M5.Axp.GetIdischargeData() / 2;
    temp = -144.7 + M5.Axp.GetTempData() * 0.1;

    const int xOff = 100;
    const int yIncr = 16;
    const int battFont = 2;
    int yOff = 0;

    M5.Lcd.setTextColor(R_GREY, BLACK);
    M5.Lcd.drawLine(xOff - 7, yOff, xOff - 7, yOff + 80, R_DGREY);
    M5.Lcd.setCursor(xOff, yOff, battFont);

    if (M5.Axp.GetWarningLeve())
        M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.printf("%.3fV\n",vbat);  //battery voltage
    M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);
    if (charge) {
        M5.Lcd.setTextColor(GREEN, BLACK);
        M5.Lcd.printf("%dmA \n",charge);  //battery charging current
        M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);
        M5.Lcd.setTextColor(R_GREY, BLACK);
    }
    if (discharge) {
        M5.Lcd.setTextColor(YELLOW, BLACK);
        M5.Lcd.printf("%dmA \n",discharge);  //battery output current
        M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);
        M5.Lcd.setTextColor(R_GREY, BLACK);
    }
    
    M5.Lcd.setTextColor(R_GREY, BLACK);
    M5.Lcd.printf("%.1fC\n",temp);  //axp192 inside temp
    M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);
    
    M5.Rtc.GetBm8563Time();
    M5.Lcd.setCursor(xOff, 65, 2);
    M5.Lcd.setTextColor(CYAN, BLACK);
    M5.Lcd.printf("%02d:%02d:%02d\n", M5.Rtc.Hour, M5.Rtc.Minute, M5.Rtc.Second);
    M5.Lcd.setTextColor(WHITE, BLACK);
}
#else
#define zwM5StickC_UpdateBatteryDisplay()
#endif

void tick(bool forceUpdate = false)
{
    if (gConfig.pauseRefresh)
        if (!forceUpdate)
            return;

    zlog("Awake at us=%lu tick=%lld\n", micros(), gSecondsSinceBoot);

#if M5STACKC
    M5.Lcd.fillScreen(TFT_BLACK);
    zwM5StickC_UpdateBatteryDisplay();
    M5.Lcd.setCursor(0, 0, 2);
#endif

    for (DisplaySpec *w = gDisplays; w->clockPin != -1 && w->dioPin != -1; w++)
        updateDisplay(w);

    _last_free = ESP.getFreeHeap();

    if (gConfig.deepSleepMode) {
        heartbeat();
        zlog("Deep-sleeping for %ds...\n", gConfig.refresh);
        Serial.flush();
        esp_sleep_enable_timer_wakeup(gConfig.refresh * 1e6);
        esp_deep_sleep_start();
    }
}

void IRAM_ATTR __isr()
{
    portENTER_CRITICAL(&__isrMutex);
    ++__isrCount;
    portEXIT_CRITICAL(&__isrMutex);
}

void loop()
{
    if (__isrCount)
    {
        portENTER_CRITICAL(&__isrMutex);
        --__isrCount;
        portEXIT_CRITICAL(&__isrMutex);
        ++gSecondsSinceBoot;
        dprint("%c%s", !(gSecondsSinceBoot % 5) ? '|' : '.', gSecondsSinceBoot % gConfig.refresh ? "" : "\n");
        zwM5StickC_UpdateBatteryDisplay();
    }

    if (!(gSecondsSinceBoot % gConfig.refresh) && gLastRefreshTick != gSecondsSinceBoot)
    {
        gLastRefreshTick = gSecondsSinceBoot;
        readConfigAndUserKeys();
        tick();
        heartbeat();
    }
}

void setup()
{
#if M5STACKC
    M5.begin();
    pinMode(M5_BUTTON_HOME, INPUT_PULLUP);
    zlog("Built for M5StickC\n");
#else
    pinMode(LED_BLTIN, OUTPUT);
    Serial.begin(SER_BAUD);
#endif

    verifyProvisioning();

    zlog("\n%s v" ZEROWATCH_VER " starting...\n", gHostname.c_str());
#if DEEP_SLEEP_MODE_ENABLE
    zlog("Deep sleep mode enabled by default\n");
#endif

    if (!(gDisplays = zwdisplayInit(gHostname)))
    {
        dprint("Display init failed, halting forever\n");
        __haltOrCatchFire();
    }
    
    if (!gConfig.deepSleepMode) {
        auto verNum = String(ZEROWATCH_VER);
        verNum.replace(".", "");
        gDisplays[0].disp->showNumberDec(verNum.toInt(), true);
        delay(2000);
    }

    if (!zwWiFiInit(gHostname.c_str(), gConfig))
    {
        dprint("WiFi init failed, halting forever\n");
        __haltOrCatchFire();
    }

    ZWRedisHostConfig redisConfig = {
        .host = EEPROMCFG_RedisHost,
        .port = EEPROMCFG_RedisPort,
        .password = EEPROMCFG_RedisPass};

    gRedis = new ZWRedis(gHostname, redisConfig);

#define NUM_RETRIES 5
    int redisConnectRetries = NUM_RETRIES;
    float redisWaitRetryTime = 50;
    float redisWaitRetryBackoffMult = 1.37;

    int errnos[NUM_RETRIES];
    while (!gRedis->connect() && --redisConnectRetries) 
    {
        // seen: ECONNABORTED (makes sense)
        errnos[NUM_RETRIES - (redisConnectRetries + 1)] = errno;
        zlog("Redis connect failed but %d retries left, waiting %0.2fs and trying again (m=%0.3f)\n", 
            redisConnectRetries, redisWaitRetryTime, redisWaitRetryBackoffMult);
        redisWaitRetryTime *= redisWaitRetryBackoffMult;
        redisWaitRetryBackoffMult *= redisWaitRetryBackoffMult;
        delay(redisWaitRetryTime);
    }

    if (!redisConnectRetries)
    {
        zlog("ERROR: redis init failed!\n");
        __haltOrCatchFire();
    }

    if (redisConnectRetries != NUM_RETRIES)
    {
        String seenErrnos = "";
        for (int i = 0; i < NUM_RETRIES && errnos[i]; i++)
            seenErrnos += String(errnos[i]) + " ";
        zlog("Redis connection had to be retried %d times. Saw: %s\n", 
            NUM_RETRIES - redisConnectRetries, seenErrnos.c_str());
        gRedis->logCritical("Redis connection had to be retried %d times. Saw: %s", 
            NUM_RETRIES - redisConnectRetries, seenErrnos.c_str());
    }

    zlog("Redis connection established, reading config...\n");

    readAndSetTime();

    readConfigAndUserKeys();

    zlog("Fully initialized! (debug %sabled)\n", gConfig.debug ? "en" : "dis");
    
    if (gConfig.debug && !gConfig.deepSleepMode)
        delay(5000);

    gPublishLogsEmit = redis_publish_logs_emit;

    __isrTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(__isrTimer, &__isr, true);
    timerAlarmWrite(__isrTimer, 1000000, true);
    timerAlarmEnable(__isrTimer);

    gBootCount = gRedis->incrementBootcount();
    zlog("%s v" ZEROWATCH_VER " up & running\n", gHostname.c_str());
    zlog("Boot count: %lu\n", gBootCount);

    tick(true);
}
