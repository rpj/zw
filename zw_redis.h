#ifndef __ZW_REDIS__H__
#define __ZW_REDIS__H__

#include <Redis.h>
#include <WiFiClient.h>
#include <vector>

#include "zw_common.h"

#define ZWREDIS_DEFAULT_EXPIRY 120

struct ZWRedisHostConfig
{
    const char *host;
    uint16_t port;
    const char *password;
};

class ZWRedis;

class ZWRedisResponder {
protected:
    ZWRedis& redis;
    String key;
    int expire = ZWREDIS_DEFAULT_EXPIRY;

public:
    ZWRedisResponder(ZWRedis& parent, String currentKey) : 
        redis(parent), key(currentKey) {}

    ~ZWRedisResponder() {}

    ZWRedisResponder(const ZWRedisResponder &) = delete;
    ZWRedisResponder &operator=(const ZWRedisResponder &) = delete;

    void setExpire(int newExpire) { expire = newExpire; }

    void setValue(const char* format, ...);
};

typedef bool (*ZWRedisUserKeyHandler)(String& userKeyValue, ZWRedisResponder& responder);

class ZWRedis {
protected:
    friend class ZWRedisResponder;

    struct RedisClientConn
    {
        Redis *redis;
        WiFiClient *wifi;
    };

    String &hostname;
    ZWRedisHostConfig configuration;
    RedisClientConn connection;

    void responderHelper(const char* key, const char* msg, int expire = 0);

public:
    ZWRedis(String &hostname, ZWRedisHostConfig config) : 
        hostname(hostname), configuration(config)
    {}

    ~ZWRedis() {}

    ZWRedis(const ZWRedis &) = delete;
    ZWRedis &operator=(const ZWRedis &) = delete;

    // TODO moves

    bool connect();

    void checkin(
        unsigned long ticks,
        const char* localIp,
        unsigned long immediateLatency,
        unsigned long averageLatency,
        int expireMessage = 60);

    bool heartbeat(int expire = 0);

    int incrementBootcount(bool reset = false);

    ZWAppConfig readConfig();

    int updateConfig(ZWAppConfig newConfig);

    bool handleUserKey(const char *keyPostfix, ZWRedisUserKeyHandler handler);

    void publishLog(const char* msg);

    bool postCompletedUpdate();

    std::vector<String> getRange(const char* key, int start, int stop);

    bool clearControlPoint();

    bool registerDevice(const char* registryName, const char* hostname, const char* ident);

private:
    ZWAppConfig _lastReadConfig;
};

#endif
