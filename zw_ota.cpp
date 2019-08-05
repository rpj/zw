#include "zw_ota.h"
#include "zw_logging.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>

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

bool runUpdate(
    const char *url, 
    const char *md5, 
    size_t sizeInBytes, 
    void (*preUpdateIRQDisable)(),
    bool (*completedCallback)())
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

                if (preUpdateIRQDisable)
                    preUpdateIRQDisable();

                dprint("OTA start szb=%d\n", sizeInBytes);
                auto updateTook = Update.writeStream(dataStream);
                if (updateTook == sizeInBytes && !Update.hasError())
                {
                    if (completedCallback && !completedCallback())
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