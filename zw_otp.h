#ifndef __ZW_OTP__H__
#define __ZW_OTP__H__

#include <Arduino.h>

#define OTP_WINDOW_MINUTES 2

// to generate anew, use ./scripts/otp-generate.pl

bool otpCheck(uint16_t otp);

#endif