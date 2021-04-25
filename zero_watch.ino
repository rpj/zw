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
#define DEF_BRIGHT 2
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
    .deepSleepMode = DEEP_SLEEP_MODE_ENABLE};
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
            "\"sketchSize\": %d, \"sdk\": \"%s\", \"chipRev\": %d, \"eFuse\": %d}",
            ZEROWATCH_VER, ESP.getSketchMD5().c_str(), ESP.getSketchSize(),
            ESP.getSdkVersion(), ESP.getChipRevision(), ESP.getEfuseMac());
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
    char *jbuf = (char *)malloc(pubLen);
    bzero(jbuf, pubLen);
    snprintf(jbuf, pubLen, PUB_FMT_STR, gHostname.c_str(), tickStr.c_str(), buf);
    gRedis->publishLog(jbuf);
    free(jbuf);
}

#if M5STACKC
void M5Stack_publish_logs_emit(const char *fmt, ...)
{
    char buf[1024];
    bzero(buf, 1024);
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);

    auto len = strlen(buf);

    if (buf[len - 1] == '\n')
        buf[len - 1] = '\0';

    M5.Lcd.println(buf);
    Serial.println(buf);
}
#endif

#if M5STACKC
void readAndSetTime()
{
    RTC_TimeTypeDef ts;
    bzero(&ts, sizeof(ts));
    gRedis->getTime(&ts.Hours, &ts.Minutes, &ts.Seconds);
    if (!(!ts.Hours && !ts.Minutes && !ts.Seconds))
    {
        M5.Rtc.SetTime(&ts);
        zlog("Set time: %02d:%02d\n", ts.Hours, ts.Minutes);
    }
    else
    {
        zlog("Failed to get time from Redis (or it is exactly midnight!)\n");
    }
}
#else
// TODO need to make this functional for all other devices!!
#define readAndSetTime()
#endif

void readConfigAndUserKeys()
{
    readAndSetTime();

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
    /*UPDATE_IF_CHANGED_ELSE_MARKED_DIRTY_WITH_EXTRAEXTRA(brightness,
                                                        curCfg.brightness >= 0 && curCfg.brightness < 8,
                                                        M5.Axp.ScreenBreath(gConfig.brightness + 7));*/
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

#if M5STACKC
int xLineOff = 148;
int yLineOffTop = 5;
int yLineOffBot = 31;
auto brightLvl = gConfig.brightness * 3;
auto borderClr = DARKGREY;
auto barClr = DARKCYAN;

uint16_t brightIcon[8][6] = {
    { BLACK, BLACK, BLACK, BLACK, BLACK, BLACK },
    { BLACK, BLACK, BLACK, BLACK, BLACK, BLACK },
    { DARKGREY, DARKGREY, DARKGREY, DARKGREY, DARKGREY, DARKGREY },
    { DARKGREY, DARKGREY, DARKGREY, DARKGREY, DARKGREY, DARKGREY },
    { LIGHTGREY, LIGHTGREY, LIGHTGREY, LIGHTGREY, LIGHTGREY, LIGHTGREY },
    { LIGHTGREY, LIGHTGREY, LIGHTGREY, LIGHTGREY, LIGHTGREY, LIGHTGREY },
    { WHITE, WHITE, WHITE, WHITE, WHITE, WHITE },
    { WHITE, WHITE, WHITE, WHITE, WHITE, WHITE }
};

void zwM5StickC_DrawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t* bitmap)
{
    for (int x_w = x; x_w < (x + w); x_w++) {
        for (int y_w = y; y_w < (y + h); y_w++) {
            M5.Lcd.drawPixel(x_w + x, y_w + y, *((bitmap + (x_w - x) * h) + (y_w - y)));
        }
    }
}

void zwM5StickC_DrawBrightnessMeterBorder()
{
    M5.Lcd.drawLine(xLineOff,   yLineOffTop, xLineOff+5, yLineOffTop, borderClr);
    M5.Lcd.drawLine(xLineOff,   yLineOffBot, xLineOff+5, yLineOffBot, borderClr);
    M5.Lcd.drawLine(xLineOff,   yLineOffTop, xLineOff,   yLineOffBot, borderClr);
    M5.Lcd.drawLine(xLineOff+5, yLineOffTop, xLineOff+5, yLineOffBot, borderClr);
    //zwM5StickC_DrawBitmap(xLineOff-2, yLineOffBot+2, 8, 8, (uint16_t*)brightIcon);
}

void zwM5StickC_UpdateBrightnessMeter()
{
    auto brightLvl = gConfig.brightness * 3;
    M5.Lcd.drawLine(xLineOff+2, yLineOffTop+1, xLineOff+2, yLineOffBot-1, BLACK);
    M5.Lcd.drawLine(xLineOff+3, yLineOffTop+1, xLineOff+3, yLineOffBot-1, BLACK);
    auto tmpY = yLineOffBot - 2;
    M5.Lcd.drawLine(xLineOff+2, tmpY - brightLvl, xLineOff+2, tmpY, barClr);
    M5.Lcd.drawLine(xLineOff+3, tmpY - brightLvl, xLineOff+3, tmpY, barClr);
    zwM5StickC_DrawBrightnessMeterBorder();
}

void zwM5StickC_UpdateBatteryDisplay()
{
    double vbat = 0.0;
    int discharge, charge;
    double temp = 0.0;

    vbat = M5.Axp.GetVbatData() * 1.1 / 1000;
    charge = M5.Axp.GetIchargeData() / 2;
    discharge = M5.Axp.GetIdischargeData() / 2;
    temp = -144.7 + M5.Axp.GetTempData() * 0.1;

    const int xOff = 94;
    const int yIncr = 16;
    const int battFont = 2;
    int yOff = 0;

    M5.Lcd.setTextColor(LIGHTGREY, BLACK);
    M5.Lcd.drawLine(xOff - 8, yOff, xOff - 8, yOff + 80, DARKCYAN);
    M5.Lcd.drawLine(xOff - 6, yOff, xOff - 6, yOff + 80, DARKCYAN);
    M5.Lcd.setCursor(xOff, yOff, battFont);

    uint16_t voltageColor = RED;
    if (!M5.Axp.GetWarningLeve())
        voltageColor = vbat > 3.9 ? GREEN : (vbat > 3.7 ? YELLOW : ORANGE);
    M5.Lcd.setTextColor(voltageColor, BLACK);
    M5.Lcd.printf("%.3fV\n", vbat); //battery voltage
    M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);

    M5.Lcd.setTextColor(LIGHTGREY, BLACK);
    M5.Lcd.printf("%.1fC\n", temp); //axp192 inside temp
    M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);

    if (charge)
    {
        M5.Lcd.setTextColor(GREEN, BLACK);
        M5.Lcd.printf("%dmA \n", charge); //battery charging current
        M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);
        M5.Lcd.setTextColor(LIGHTGREY, BLACK);
    }
    if (discharge)
    {
        M5.Lcd.setTextColor(ORANGE, BLACK);
        M5.Lcd.printf("%dmA \n", discharge); //battery output current
        M5.Lcd.setCursor(xOff, yOff += yIncr, battFont);
        M5.Lcd.setTextColor(LIGHTGREY, BLACK);
    }

    M5.Rtc.GetBm8563Time();
    M5.Lcd.setCursor(xOff, 65 - 8, 4);
    M5.Lcd.setTextColor(CYAN, BLACK);
    M5.Lcd.printf("%02d:%02d\n", M5.Rtc.Hour % 12, M5.Rtc.Minute);
    M5.Lcd.setTextColor(WHITE, BLACK);

    zwM5StickC_UpdateBrightnessMeter();
}
#else
#define zwM5StickC_UpdateBrightnessMeter()
#define zwM5StickC_DrawBrightnessMeterBorder()
#define zwM5StickC_UpdateBatteryDisplay()
#endif

#define PAGE_SIZE 4
static int __dispPage = 0;
static int __dispPages = 0;

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

    for (DisplaySpec *w = (gDisplays + (__dispPage * PAGE_SIZE));
         (w - (gDisplays + (__dispPage * PAGE_SIZE))) < PAGE_SIZE && (w->clockPin != -1 && w->dioPin != -1);
         w++) 
            updateDisplay(w);

#if M5STACKC
    M5.Lcd.setTextColor(NAVY, BLACK);
    M5.Lcd.printf("          <%d>\n", __dispPage + 1);
#endif

    _last_free = ESP.getFreeHeap();

    if (gConfig.deepSleepMode)
    {
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

static bool __lastHome = true;
static bool __lastRst = true;
static uint64_t __rstDebounce = 0;
void loop()
{
    if (__isrCount)
    {
        portENTER_CRITICAL(&__isrMutex);
        --__isrCount;
        portEXIT_CRITICAL(&__isrMutex);
        ++gSecondsSinceBoot;
        zwM5StickC_UpdateBatteryDisplay();
    }

    auto curHome = digitalRead(M5_BUTTON_HOME);
    auto curRst = digitalRead(M5_BUTTON_RST);
    bool forceTick = false;

    if (curHome != __lastHome)
    {
        if (curHome && !__lastHome)
        {
            if (__dispPages)
            {
                __dispPage = (__dispPage + 1) % (__dispPages + 1);
            }

            forceTick = true;
        }
    }

    if (curRst != __lastRst)
    {
        if (curRst && !__lastRst && (!__rstDebounce || (millis() - __rstDebounce > 250)))
        {
            __rstDebounce = millis();
            M5.Axp.ScreenBreath((gConfig.brightness = (gConfig.brightness + 1) % 8) + 7);
            zwM5StickC_UpdateBrightnessMeter();
        }
    }

    __lastHome = curHome;
    __lastRst = curRst;

    if (forceTick || (!(gSecondsSinceBoot % gConfig.refresh) && gLastRefreshTick != gSecondsSinceBoot))
    {
        if (forceTick)
        {
#if M5STACKC
            M5.Lcd.fillScreen(TFT_BLACK);
#endif
        }

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
    pinMode(M5_BUTTON_RST, INPUT_PULLUP);
    zlog("Built for M5StickC\n");
    gConfig.publishLogs = true;
    gPublishLogsEmit = M5Stack_publish_logs_emit;
#else
    pinMode(LED_BLTIN, OUTPUT);
    Serial.begin(SER_BAUD);
#endif

    verifyProvisioning();

    if (!(gDisplays = zwdisplayInit(gHostname)))
    {
        dprint("Display init failed, halting forever\n");
        __haltOrCatchFire();
    }

    auto buildVariant = "";
#if M5STACKC
    buildVariant = "-M5SC";
    M5.Lcd.setCursor(0, 0, 1);

    //zwM5StickC_DrawBitmap(10, 10, 8, 8, (uint16_t*)brightIcon);
    //delay(120000);
#endif

    zlog("%s v" ZEROWATCH_VER "%s\n", gHostname.c_str(), buildVariant);

    auto dWalk = gDisplays;
    for (; dWalk->clockPin != -1 && dWalk->dioPin != -1; dWalk++);
    __dispPages = (int)((dWalk - gDisplays) / PAGE_SIZE) - (!((dWalk - gDisplays) % PAGE_SIZE) ? 1 : 0);

    if (!gConfig.deepSleepMode)
    {
#if !M5STACKC
        auto verNum = String(ZEROWATCH_VER);
        verNum.replace(".", "");
        gDisplays[0].disp->showNumberDec(verNum.toInt(), true);
        delay(2000);
#endif
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

    gBootCount = gRedis->incrementBootcount();

    zlog("Initialized! (debug %s)\n", gConfig.debug ? "on" : "off");
    zlog("Boot count: %lu\n", gBootCount);

    readConfigAndUserKeys();

    if (gConfig.debug && !gConfig.deepSleepMode)
        delay(5000);

    __isrTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(__isrTimer, &__isr, true);
    timerAlarmWrite(__isrTimer, 1000000, true);
    timerAlarmEnable(__isrTimer);

#if M5STACKC
    delay(gConfig.debug ? 10000 : 2000);
    gPublishLogsEmit = NULL;
    M5.Lcd.setCursor(0, 0, 2);
    M5.Lcd.fillScreen(TFT_BLACK);
#endif

    gPublishLogsEmit = redis_publish_logs_emit;

    tick(true);
}
