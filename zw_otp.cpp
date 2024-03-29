#include "zw_otp.h"
#include "zw_logging.h"

extern String gHostname;

// the fudge table is use strictly to make a linear sequence appear
// non-linear over a short observation time period. security by obscurity ftw!
static const uint8_t fudgeTable[] = {42, 69, 3, 18, 25, 12, 51, 93, 54, 76};
static const uint8_t fudgeTableLen = 10;

// to generate anew, use ./scripts/otp-generate.pl
bool otpCheck(uint16_t otp)
{
    auto div = (unsigned long)1e6 * 60 * OTP_WINDOW_MINUTES;
    auto now = micros();

    if (now < div)
    {
        dprint("Can't calculate OTPs yet, must be running for at least %d minutes\n", OTP_WINDOW_MINUTES);
        return false;
    }

    auto internalChecker = micros() / div;
    internalChecker += fudgeTable[internalChecker % fudgeTableLen];

    for (int i = 0; i < gHostname.length(); i++)
    {
        internalChecker += gHostname.charAt(i);
    }

    return (uint16_t)internalChecker == otp;
}