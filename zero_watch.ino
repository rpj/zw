// zero_watch.ino
// (C) Ryan Joseph, 2019 <ryan@electricsheep.co>

#include <WiFiClient.h>
#include <WiFi.h>
#include <Update.h>
#include <HTTPClient.h>
#include <Redis.h>
#include <TM1637Display.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

#include "zw_common.h"
#include "zw_logging.h"
#include "zw_redis.h"
#include "zw_provision.h"

#define OTA_RESET_DELAY 5
#define OTA_UPDATE_PRCNT_REPORT 10
#define OTP_WINDOW_MINUTES 2
#define CONTROL_POINT_SEP_CHAR '#'
#define LED_BLTIN_H LOW
#define LED_BLTIN_L HIGH
#define LED_BLTIN 2
#define SER_BAUD 115200
#define DEF_BRIGHT 0
#define HB_CHECKIN 5
#define LAT_FUNC micros
#if DEBUG
#define DEF_REFRESH 20
#else
#define DEF_REFRESH 240
#endif

class DisplaySpec;

#define EXEC_ALL_DISPS(EXEC_ME)                                                                 \
    do                                                                                          \
    {                                                                                           \
        for (DisplaySpec *walk = gDisplays; walk->clockPin != -1 && walk->dioPin != -1; walk++) \
            walk->disp->EXEC_ME;                                                                \
    } while (0)

#define EXEC_WITH_EACH_DISP(EFUNC)                                                              \
    do                                                                                          \
    {                                                                                           \
        for (DisplaySpec *walk = gDisplays; walk->clockPin != -1 && walk->dioPin != -1; walk++) \
            EFUNC(walk->disp);                                                                  \
    } while (0)

struct InfoSpec
{
    const char *listKey;
    int startIdx;
    int endIdx;
    double lastTs;
    int lastVal;
    std::function<int(int)> adjFunc;
    std::function<void(DisplaySpec *)> dispFunc;
};

struct DisplaySpec
{
    int clockPin;
    int dioPin;
    TM1637Display *disp;
    InfoSpec spec;
};

ZWAppConfig gConfig = {
    .brightness = DEF_BRIGHT,
    .refresh = DEF_REFRESH,
    .debug = DEBUG,
    .publishLogs = false,
    .pauseRefresh = false};
ZWRedis *gRedis;
DisplaySpec *gDisplays;
void (*gPublishLogsEmit)(const char *fmt, ...);
unsigned long __lc = 0;
int _last_free = 0;
unsigned long gUDRA = 0;
unsigned long immediateLatency = 0;
StaticJsonBuffer<1024> jsonBuf;

int noop(int a) { return a; }

void d_def(DisplaySpec *d) { d->disp->showNumberDec(d->spec.lastVal); }

static uint8_t degFSegs[] = {99, 113};
static uint8_t fSeg[] = {113};
static uint8_t prcntSeg[] = {99, 92};
void d_tempf(DisplaySpec *d)
{
    if (d->spec.lastVal < 10000)
    {
        d->disp->showNumberDecEx(d->spec.lastVal, 0, false);
        d->disp->setSegments(degFSegs, 2, 2);
    }
    else
    {
        d->disp->showNumberDecEx(d->spec.lastVal / 100, 0, false, 3);
        d->disp->setSegments(fSeg, 1, 3);
    }
}

void d_humidPercent(DisplaySpec *d)
{
    d->disp->showNumberDecEx(d->spec.lastVal, 0, false);
    d->disp->setSegments(prcntSeg, 2, 2);
}

DisplaySpec gDisplays_AMINI[] = {
    {33, 32, nullptr, {"zero:sensor:BME280:temperature:.list", 0, 11, 0.0, 0, noop, d_tempf}},
    {26, 25, nullptr, {"zero:sensor:BME280:humidity:.list", 0, 11, 0.0, 0, noop, d_humidPercent}},
    {18, 19, nullptr, {"zero:sensor:BME280:pressure:.list", 0, 5, 0.0, 0, [](int i) { return i / 100; }, d_def}},
    {-1, -1, nullptr, {nullptr, -1, -1, -1.0, -1, noop, d_def}}};

DisplaySpec gDisplays_EZERO[] = {
    {33, 32, nullptr, {"zero:sensor:BME280:pressure:.list", 0, 5, 0.0, 0, [](int i) { return i / 100; }, d_def}},
    {18, 19, nullptr, {"zero:sensor:BME280:temperature:.list", 0, 11, 0.0, 0, noop, d_tempf}},
    {26, 25, nullptr, {"zed:sensor:SPS30:mc_2p5:.list", 0, 5, 0.0, 0, [](int i) { return i / 100; }, d_def}},
    {13, 14, nullptr, {"zero:sensor:BME280:humidity:.list", 0, 11, 0.0, 0, noop, d_humidPercent}},
    {-1, -1, nullptr, {nullptr, -1, -1, -1.0, -1, noop, d_def}}};

void blink(int d = 50)
{
    digitalWrite(LED_BLTIN, LED_BLTIN_H);
    delay(d);
    digitalWrite(LED_BLTIN, LED_BLTIN_L);
    delay(d);
    digitalWrite(LED_BLTIN, LED_BLTIN_H);
}

struct AnimStep
{
    int digit;
    int bits;
};

AnimStep full_loop[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {3, 3}, {3, 7}, 
    {3, 15}, {2, 9}, {1, 9}, {0, 9}, {0, 25}, {0, 57}, {-1, -1}};

AnimStep light_loop[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {3, 2}, {3, 4}, 
    {3, 8}, {2, 8}, {1, 8}, {0, 8}, {0, 16}, {0, 32}, {-1, -1}};

void run_animation(TM1637Display *d, AnimStep *anim, bool cE = false, int s = 0)
{
    uint8_t v[] = {0, 0, 0, 0};
    for (AnimStep *w = anim; w->bits != -1 && w->digit != -1; w++)
    {
        if (cE)
            bzero(v, 4);
        v[w->digit] = w->bits;
        d->setSegments(v);
        if (s)
            delay(s);
    }
}

bool tmdisplay_init()
{
    zlog("Initializing displays with brightness level %d\n", gConfig.brightness);
    DisplaySpec *spec = gDisplays;
    for (; spec->clockPin != -1 && spec->dioPin != -1; spec++)
    {
        zlog("Setting up display #%d with clock=%d DIO=%d\n",
             (int)(spec - gDisplays), spec->clockPin, spec->dioPin);
        spec->disp = new TM1637Display(spec->clockPin, spec->dioPin);
        spec->disp->clear();
        spec->disp->setBrightness(gConfig.brightness);
        run_animation(spec->disp, full_loop, false, 5);
    }

    if (gConfig.debug)
    {
        EXEC_ALL_DISPS(showNumberDecEx((int)(walk - gDisplays), 0,
                                       false, 4 - (int)(walk - gDisplays), (int)(walk - gDisplays)));
        delay(2000);
    }

    return true;
}

bool wifi_init()
{
    dprint("Disabling WiFi AP\n");
    WiFi.mode(WIFI_MODE_STA);
    WiFi.enableAP(false);

    auto bstat = WiFi.begin(EEPROMCFG_WiFiSSID, EEPROMCFG_WiFiPass);
    dprint("Connecting to to '%s'...\n", EEPROMCFG_WiFiSSID);
    dprint("WiFi.begin() -> %d\n", bstat);

    run_animation(gDisplays[0].disp, light_loop, true);
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

void updateDisplay(DisplaySpec *disp)
{
    if (gConfig.debug)
        run_animation(disp->disp, full_loop);

    auto __s = LAT_FUNC();
    auto lrVec = gRedis->getRange(disp->spec.listKey, disp->spec.startIdx, disp->spec.endIdx);
    immediateLatency = LAT_FUNC() - __s;
    auto newUDRA = gUDRA == 0 ? immediateLatency : (gUDRA + immediateLatency) / 2;
    auto deltaUDRA = newUDRA - gUDRA;
    gUDRA = newUDRA;

    if (lrVec.size())
    {
        double acc = 0.0;
        for (auto lrStr : lrVec)
        {
            if (lrStr.length() < 256)
            {
                jsonBuf.clear();
                JsonArray &jsRoot = jsonBuf.parseArray(lrStr.c_str());
                disp->spec.lastTs = (double)jsRoot[0];
                acc += (double)jsRoot[1];
            }
        }

        if (gConfig.debug)
            run_animation(disp->disp, light_loop, true);

        disp->spec.lastVal = disp->spec.adjFunc((int)((acc * 100.0) / lrVec.size()));
        disp->spec.dispFunc(disp);
        zlog("[%s] count %d val %d immLat %lu gUDRA %lu (delta %ld)\n",
             disp->spec.listKey, lrVec.size(), disp->spec.lastVal, immediateLatency, gUDRA, deltaUDRA);
    }
}

void demoForDisp(TM1637Display *disp)
{
    run_animation(disp, full_loop);
    run_animation(disp, light_loop);
    run_animation(disp, full_loop);
    run_animation(disp, light_loop);
    run_animation(disp, full_loop);
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
        dprint("Demo! Bleep bloop!\n");
        zlog("Demo! Bleep bloop!\n");
        EXEC_WITH_EACH_DISP(demoForDisp);
    }
    else if (imEmit.equals("latency"))
    {
        responder.setValue("{ \"immediate\": %d, \"rollingAvg\": %d }", immediateLatency, gUDRA);
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

void updateProg(size_t s1, size_t s2)
{
    static int lastUpdate = 0;
    auto curPercent = ((double)s1 / s2) * 100.0;
    if ((unsigned)curPercent >= lastUpdate + OTA_UPDATE_PRCNT_REPORT)
    {
        lastUpdate = (unsigned)curPercent;
        dprint("%d.. %s", lastUpdate, (lastUpdate == 100 ? "\n" : ""));
        zlog("OTA update progress: %0.2f%% (%d)\n", curPercent, s1);
    }
}

bool runUpdate(const char *url, const char *md5, size_t sizeInBytes)
{
    HTTPClient http;
    if (http.begin(url))
    {
        auto code = http.GET();
        if (code > 0)
        {
            auto dataStream = http.getStream();
            auto avail = dataStream.available();
            if (avail == 0)
            {
                zlog("ERROR: no bytes available!\n");
                return false;
            }

            if (Update.begin(sizeInBytes))
            {
                Update.onProgress(updateProg);
                Update.setMD5(md5);
                dprint("OTA start szb=%d\n", sizeInBytes);
                auto updateTook = Update.writeStream(dataStream);
                if (updateTook == sizeInBytes && !Update.hasError())
                {
                    if (!gRedis->postCompletedUpdate())
                    {
                        zlog("WARNING: unable to delete update key!\n");
                    }

                    if (!Update.end())
                    {
                        zlog("UPDATE END FAILED!? WTF mate\n");
                        Update.abort();
                        return false;
                    }

                    return true;
                }
                else
                {
                    zlog("UPDATE FAILED: %d\n", Update.getError());
                    Update.abort();
                }
            }
            else
            {
                zlog("UPDATE couldn't start: %d\n", Update.getError());
                Update.abort();
            }
        }
        else
        {
            zlog("HTTPClient.get() failed: %d\n", code);
        }
    }
    else
    {
        zlog("HTTPClient.begin() failed\n");
    }

    return false;
}

// the fudge table is use strictly to make a linear sequence appear
// non-linear over a short observation time period. security by obscurity ftw!
static const uint8_t fudgeTable[] = {42, 69, 3, 18, 25, 12, 51, 93, 54, 76};
static const uint8_t fudgeTableLen = 10;

// to generate anew, use ./scripts/otp-generate.pl
bool otpCheck(uint16_t otp)
{
    dprint("[OTP] A: %d\n", otp);
    dprint("[OTP] I: %ld\n", micros());
    auto div = (unsigned long)1e6 * 60 * OTP_WINDOW_MINUTES;
    auto now = micros();

    if (now < div)
    {
        dprint("Can't calculate OTPs yet, must be running for at least %d minutes\n", OTP_WINDOW_MINUTES);
        return false;
    }

    auto internalChecker = micros() / div;
    dprint("[OTP] 0: %ld\n", internalChecker);

    internalChecker += fudgeTable[internalChecker % fudgeTableLen];
    dprint("[OTP] D: %ld\n", internalChecker);

    for (int i = 0; i < gHostname.length(); i++)
    {
        internalChecker += gHostname.charAt(i);
        dprint("[OTP] %d: %ld (+= %d)\n", i, internalChecker, (int)gHostname.charAt(i));
    }

    dprint("[OTP] F: %d\n", (uint16_t)internalChecker);
    return (uint16_t)internalChecker == otp;
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
            if (runUpdate(fqUrl, md5, szb))
            {
                zlog("OTA update wrote successfully! Restarting in %d seconds...\n", OTA_RESET_DELAY);
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
        EXEC_ALL_DISPS(setBrightness(gConfig.brightness));
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
            gRedis->checkin(__lc, WiFi.localIP().toString().c_str(), immediateLatency, gUDRA, gConfig.refresh * 5);
        }
    }
}

void tick(bool forceUpdate = false)
{
    if (gConfig.pauseRefresh)
        if (!forceUpdate)
            return;

    zlog("Awake at us=%lu tick=%ld\n", micros(), __lc);

    for (DisplaySpec *w = gDisplays; w->clockPin != -1 && w->dioPin != -1; w++)
        updateDisplay(w);

    _last_free = ESP.getFreeHeap();
}

void loop()
{
    if (!(__lc++ % gConfig.refresh))
    {
        readConfigAndUserKeys();
        heartbeat();
        tick();
    }

    if (!(__lc % 5))
    {
        blink(15);
        delay(5);
        blink(15);
    }
    dprint("%c%s", !(__lc % 5) ? '|' : '.', __lc % gConfig.refresh ? "" : "\n");
    delay(900);
}

void setup()
{
    pinMode(LED_BLTIN, OUTPUT);
    Serial.begin(SER_BAUD);
    
    verifyProvisioning();

    if (gHostname.equals("ezero"))
    {
        gDisplays = gDisplays_EZERO;
    }
    else if (gHostname.equals("amini"))
    {
        gDisplays = gDisplays_AMINI;
    }

    zlog("\n%s v" ZEROWATCH_VER " starting...\n", gHostname.c_str());

    if (tmdisplay_init() && wifi_init())
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

            if (gConfig.pauseRefresh)
            {
                zlog("Running tick 0 forcefully because refresh is paused at init\n");
                tick(true);
            }

            zlog("Fully initialized! (debug %sabled)\n", gConfig.debug ? "en" : "dis");

            if (gConfig.debug)
                delay(5000);

            gPublishLogsEmit = redis_publish_logs_emit;

            zlog("Boot count: %d\n", gRedis->incrementBootcount());
            zlog("%s v" ZEROWATCH_VER " up & running\n", gHostname.c_str());
        }
        else
        {
            zlog("ERROR: redis init failed!");
        }
    }
}
