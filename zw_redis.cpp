#include "zw_redis.h"
#include "zw_logging.h"

#define REDIS_KEY(x) String(hostname + x).c_str()

#define REDIS_KEY_CREATE_LOCAL(x)          \
    auto redisKey_local__String_capture = String(hostname + x); \
    auto redisKey_local = redisKey_local__String_capture.c_str();

bool ZWRedis::connect()
{
    connection.wifi = new WiFiClient();

    if (!connection.wifi->connect(configuration.host, configuration.port))
    {
        dprint("Redis connection failed");
        delete connection.wifi, connection.wifi = nullptr;
        return false;
    }
    else
    {
        connection.redis = new Redis(*connection.wifi);
        if (connection.redis->authenticate(configuration.password) != RedisSuccess)
        {
            dprint("Redis auth failed");
            delete connection.redis, connection.redis = nullptr;
            return false;
        }
    }

    return true;
}

void ZWRedis::checkin(
    unsigned long ticks,
    const char* localIp,
    unsigned long immediateLatency,
    unsigned long averageLatency,
    int expireMessage)
{
    auto rKey = String("rpjios.checkin." + hostname);
    const char *key = rKey.c_str();

    // TODO: error check!
    connection.redis->hset(key, "host", hostname.c_str());
    connection.redis->hset(key, "up", String(ticks).c_str());
    connection.redis->hset(key, "ver", ZEROWATCH_VER);
    char _ifbuf[1024];
    bzero(_ifbuf, 1024);
    snprintf(_ifbuf, 1024,
             "{ \"wifi\": { \"address\": \"%s\", \"latency\": "
             "{ \"immediate\": %ld, \"rollingAvg\": %ld }}}",
             localIp, immediateLatency, averageLatency);
    connection.redis->hset(key, "ifaces", _ifbuf);
    connection.redis->expire(key, expireMessage);
}

bool ZWRedis::heartbeat(int expire)
{
    REDIS_KEY_CREATE_LOCAL(":heartbeat");
    if (!connection.redis->set(redisKey_local, String(micros()).c_str()))
        return false;

    if (expire)
        connection.redis->expire(redisKey_local, expire);

    return true;
}

int ZWRedis::incrementBootcount(bool reset)
{
    REDIS_KEY_CREATE_LOCAL(":bootcount");
    auto bc = connection.redis->get(redisKey_local);
    auto bcNext = (reset ? 0 : bc.toInt()) + 1;

    if (connection.redis->set(redisKey_local, String(bcNext).c_str())) {
        return bcNext;
    }
    
    return -1;
}

ZWAppConfig ZWRedis::readConfig()
{
    ZWAppConfig retCfg;

    // TODO: error check!
    auto bc = connection.redis->get(REDIS_KEY(":config:brightness"));
    auto rc = connection.redis->get(REDIS_KEY(":config:refresh"));
    auto dg = connection.redis->get(REDIS_KEY(":config:debug"));
    auto pl = connection.redis->get(REDIS_KEY(":config:publishLogs"));
    auto pu = connection.redis->get(REDIS_KEY(":config:pauseRefresh"));

    retCfg.brightness = bc.toInt();
    retCfg.refresh = rc.toInt();
    retCfg.debug = (bool)dg.toInt();
    retCfg.publishLogs = (bool)pl.toInt();
    retCfg.pauseRefresh = (bool)pu.toInt();

    return retCfg;
}

bool ZWRedis::handleUserKey(const char *keyPostfix, ZWRedisUserKeyHandler handler)
{
    if (!keyPostfix || !handler)
    {
        zlog("ZWRedis::handleUserKey ERROR arguments\n");
        return false;
    }

    auto getReturn = connection.redis->get(REDIS_KEY(keyPostfix));

    if (getReturn && getReturn.length()) {
        REDIS_KEY_CREATE_LOCAL(keyPostfix + ":" + getReturn);
        ///
        // TODO: handle this wierd print on things like 'update'...
        // and make sure they never write to keys like that!
        // e.g.
        // ZWRedis::handleUserKey(ezero) (key=:config:update) has return path 'ezero:config:update:{
        //  "url":  "zero_watch_updates/zero_watch-v0.2.0.4.ino.bin",
        //  "md5":  "eb8a0182161c88328c4888cc64e8a822",
        //  "size": 924368,
        //  "otp": 619
        //  }'"
        ///
        dprint("ZWRedis::handleUserKey(%s) (key=%s) has return path '%s'\n", 
            hostname.c_str(), keyPostfix, redisKey_local);
        ZWRedisResponder responder(*this, redisKey_local__String_capture);
        if (handler(getReturn, responder))
        {
            return connection.redis->del(REDIS_KEY(keyPostfix));
        }
    }

    return false;
}

void ZWRedis::responderHelper(const char *key, const char *msg, int expire)
{
    connection.redis->publish(key, msg);
    if (!connection.redis->set(key, msg)) {
        zlog("ERROR: ZWRedis::responderHelper() set of %s failed\n", key);
        return;
    }

    if (expire > 0) {
        dprint("ZWRedis::responderHelper expiring %s at %d\n", key, expire);
        connection.redis->expire(key, expire);
    }
}

void ZWRedis::publishLog(const char* msg)
{
    connection.redis->publish(REDIS_KEY(":info:publishLogs"), msg);
}

bool ZWRedis::postCompletedUpdate()
{
    return connection.redis->del(REDIS_KEY(":config:update"));
}

std::vector<String> ZWRedis::getRange(const char* key, int start, int stop)
{
    return connection.redis->lrange(key, start, stop);
}

bool ZWRedis::clearControlPoint()
{
    return connection.redis->del(REDIS_KEY(":config:controlPoint"));
}

void ZWRedisResponder::setValue(const char *format, ...)
{
#define BUFLEN 1024
    char _buf[1024];
    bzero(_buf, 1024);
    va_list args;
    va_start(args, format);
    vsnprintf(_buf, BUFLEN, format, args);
    redis.responderHelper(key.c_str(), _buf, expire);
    va_end(args);
}