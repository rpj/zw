#include "zw_displays.h"
#include "zw_logging.h"

// TODO: get rid of these externs! (and associated includes!)
extern unsigned long immediateLatency;
extern unsigned long gUDRA;
extern ZWAppConfig gConfig;
#include "zw_redis.h"
extern ZWRedis *gRedis;
#include <ArduinoJson.h>
extern StaticJsonBuffer<1024> jsonBuf;

struct AnimStep
{
    int digit;
    int bits;
};

AnimStep full_loop[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {3, 3}, {3, 7}, {3, 15}, {2, 9}, {1, 9}, {0, 9}, {0, 25}, {0, 57}, {-1, -1}};

AnimStep light_loop[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {3, 2}, {3, 4}, {3, 8}, {2, 8}, {1, 8}, {0, 8}, {0, 16}, {0, 32}, {-1, -1}};

void __runAnimation(TM1637Display *d, AnimStep *anim, bool cE = false, int s = 0)
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

static uint8_t degFSegs[] = {99, 113};
static uint8_t fSeg[] = {113};
static uint8_t prcntSeg[] = {99, 92};

int noop(int a) { return a; }

void d_def(DisplaySpec *d) { d->disp->showNumberDec(d->spec.lastVal); }

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

DisplaySpec gDisplays_ETEST[] = {
    {26, 25, nullptr, {"zed:sensor:SPS30:mc_2p5:.list", 0, 5, 0.0, 0, [](int i) { return i / 100; }, d_def}},
    {-1, -1, nullptr, {nullptr, -1, -1, -1.0, -1, noop, d_def}}};

DisplaySpec *zwdisplayInit(String &hostname)
{
    DisplaySpec *retSpec = NULL;

    if (hostname.equals("ezero"))
    {
        retSpec = gDisplays_EZERO;
    }
    else if (hostname.equals("amini"))
    {
        retSpec = gDisplays_AMINI;
    }
    else if (hostname.equals("etest"))
    {
        retSpec = gDisplays_ETEST;
    }
    else {
        zlog("zwdisplayInit: ERROR unconfigured hostname '%s'\n", hostname.c_str());
    }

    if (retSpec)
    {
        auto spec = retSpec;
        zlog("Initializing displays with brightness level %d\n", gConfig.brightness);
        for (; spec->clockPin != -1 && spec->dioPin != -1; spec++)
        {
            zlog("Setting up display #%d with clock=%d DIO=%d\n",
                 (int)(spec - retSpec), spec->clockPin, spec->dioPin);
            spec->disp = new TM1637Display(spec->clockPin, spec->dioPin);
            spec->disp->clear();
            spec->disp->setBrightness(gConfig.brightness);
            __runAnimation(spec->disp, full_loop, false, 5);
        }

        if (gConfig.debug)
        {
            EXEC_ALL_DISPS(retSpec, showNumberDecEx((int)(walk - retSpec), 0,
                                                    false, 4 - (int)(walk - retSpec), (int)(walk - retSpec)));
            delay(2000);
        }
    }

    return retSpec;
}

void updateDisplay(DisplaySpec *disp)
{
    if (gConfig.debug)
        __runAnimation(disp->disp, full_loop);

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
            __runAnimation(disp->disp, light_loop, true);

        disp->spec.lastVal = disp->spec.adjFunc((int)((acc * 100.0) / lrVec.size()));
        disp->spec.dispFunc(disp);
        zlog("[%s] count %d val %d immLat %lu gUDRA %lu (delta %ld)\n",
             disp->spec.listKey, lrVec.size(), disp->spec.lastVal, immediateLatency, gUDRA, deltaUDRA);
    }
}

void blink(int d)
{
    digitalWrite(LED_BLTIN, LED_BLTIN_H);
    delay(d);
    digitalWrite(LED_BLTIN, LED_BLTIN_L);
    delay(d);
    digitalWrite(LED_BLTIN, LED_BLTIN_H);
}

void runAnimation(TM1637Display *d, String animation, bool cE, int s)
{
    AnimStep *anim = NULL;

    if (animation.equals("full_loop"))
    {
        anim = full_loop;
    }
    else if (animation.equals("light_loop"))
    {
        anim = light_loop;
    }
    else
    {
        zlog("No animation defined for '%s'!\n", animation.c_str());
        return;
    }

    __runAnimation(d, anim, cE, s);
}

#define EXEC_WITH_EACH_DISP(DISPLIST_START, EFUNC)                                                   \
    do                                                                                               \
    {                                                                                                \
        for (DisplaySpec *walk = DISPLIST_START; walk->clockPin != -1 && walk->dioPin != -1; walk++) \
            EFUNC(walk->disp);                                                                       \
    } while (0)

void demoForDisp(TM1637Display *disp)
{
    __runAnimation(disp, full_loop);
    __runAnimation(disp, light_loop);
    __runAnimation(disp, full_loop);
    __runAnimation(disp, light_loop);
    __runAnimation(disp, full_loop);
}

void demoMode(DisplaySpec *displayListStart)
{
    dprint("Demo! Bleep bloop!\n");
    zlog("Demo! Bleep bloop!\n");
    EXEC_WITH_EACH_DISP(displayListStart, demoForDisp);
}
