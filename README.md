# ZeroWatch

A highly-configurable ESP32-based Redis-watcher and TM1637-displayer, with OTA capability.

Named as such because it was originally started to monitor "zero", my RPi Zero W.

Currently running things like [this](https://twitter.com/rpjios/status/1155609352528486400) and [this](https://twitter.com/rpjios/status/1100077092287344642):

![version 2](https://pbs.twimg.com/media/EAmMSaPVUAEKYcl?format=jpg&name=small)
![version 1](https://pbs.twimg.com/media/D0RCGcvVAAArx5x?format=jpg&name=small)

## Dependencies

* [ArduinoJson](https://arduinojson.org/) **version 5**
* [avishorp's TM1637 driver](https://github.com/avishorp/TM1637)
* [Arduino-Redis](http://arduino-redis.com/) version [2.1.1](https://github.com/electric-sheep-co/arduino-redis/releases/tag/2.1.1) or later

## Provisioning

Units must be provisioned with [critical datum](https://github.com/rpj/zw/blob/master/zw_provision.h#L10-L16), written to EEPROM, before they will behave correctly.

To do so, fill in the aforementioned fields appropriately, set [`ZERO_WATCH_PROVISIONING_MODE` to `1`](https://github.com/rpj/zw/blob/master/zw_provision.h#L7) and upload to your ESP32 while monitoring serial (at [this baud rate](https://github.com/rpj/zw/blob/master/zero_watch.ino#L18)). There are wait-points that allow you to remove power for the unit before data is written.

Most critical of these values is [hostname](https://github.com/rpj/zw/blob/master/zw_provision.h#L10), which is limited to 32 characters in length and *must be unique across your network*. All references to `HOSTNAME` elsewhere in this document refer to this data.

Once provisioned, the unit will halt forever, so to return it to normal behavior: unset `ZERO_WATCH_PROVISIONING_MODE` and all `ZWPROV_*` fields, rebuild and reflash. That's it!

## Configuration

Most of the behavior, save for the [display specifications](https://github.com/rpj/zw/blob/master/zw_displays.cpp#L67-L78) (which will one day be configurable as well), is configurable at runtime via the Redis instance the unit connects to.

Specifically, a [number of fields](https://github.com/rpj/zw/blob/master/zw_common.h#L7-L13) are exposed as `HOSTNAME:config:*` keys for which any written (valid) value [will be honored](https://github.com/rpj/zw/blob/master/zero_watch.ino#L264-L295) on the next refresh cycle.

There is also a [control point](https://github.com/rpj/zw/blob/master/zero_watch.ino#L131) key at `HOSTNAME:config:controlPoint`, a [metadata getter](https://github.com/rpj/zw/blob/master/zero_watch.ino#L69) at `HOSTNAME:config:getValue` and the [OTA update configuration](https://github.com/rpj/zw/blob/master/zero_watch.ino#L177) key at `HOSTNAME:config:update`.

## OTA

Set [`ZWPROV_OTA_HOST`](https://github.com/rpj/zw/blob/master/zw_provision.h#L16) when provisioning to an HTTP (only, currently) host visible to the unit and this will be combined with the update metadata's `url` component [to produce the fully-qualified URL](X) for acquisition of the update binary.

The aforementioned metadata must be written to `HOSTNAME:config:update` as as JSON object consisting of: `url`, `md5`, `size`, and `otp` (a ["one-time password"](https://github.com/rpj/zw/blob/master/zw_otp.cpp)), e.g.:

```js
{
    "url":  "zero_watch_updates/zero_watch-v0.2.0.6.ino.bin",
    "md5":  "1a6f92066b6e3a63362c6b376fbc438d",
    "size": 924960,
    "otp": 123
}
```

On the next refresh cycle, this data will be picked up and acted upon. Monitor serial or Redis (depending on `HOSTNAME:config:publishLogs`) for logging. Upon successful update, the unit will delete `HOSTNAME:config:update` and reset to the new software (after a [small, build-time configurable delay](https://github.com/rpj/zw/blob/master/zw_ota.h#L6)).

Pre-built images of recent versions are available [here](https://ota.rpjios.com/), but only via HTTPS so as to be unuseable as `ZWPROV_OTA_HOST`. It's highly advised you stand up your OTA within your local network, anyway! :smile:
