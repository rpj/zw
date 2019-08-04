# ZeroWatch

A highly-configurable ESP32-based Redis-watcher and TM1637-displayer, with OTA capability.

Named as such because it was originally started to monitor "zero", my RPi Zero W.

Currently running things like [this](https://twitter.com/rpjios/status/1155609352528486400) and [this](https://twitter.com/rpjios/status/1100077092287344642):

![version 2](https://pbs.twimg.com/media/EAmMSaPVUAEKYcl?format=jpg&name=small)
![version 1](https://pbs.twimg.com/media/D0RCGcvVAAArx5x?format=jpg&name=small)

## Provisioning

Units must be provisioned with [critical datum](https://github.com/rpj/zw/blob/master/zw_provision.h#L10-L16), written to EEPROM, before they will behave correctly.

To do so, fill in the aforementioned fields appropriately, set [`ZERO_WATCH_PROVISIONING_MODE` to `1`](https://github.com/rpj/zw/blob/master/zw_provision.h#L7) and upload to your ESP32 while monitoring serial (at [this baud rate](https://github.com/rpj/zw/blob/master/zero_watch.ino#L22)). There are wait-points that allow you to remove power for the unit before data is written.

Most critical of these are the [hostname](https://github.com/rpj/zw/blob/master/zw_provision.h#L10) value, which is limited to 32 characters in length and *must be unique across your network*.

Once provisioned, unset `ZERO_WATCH_PROVISIONING_MODE`, rebuild and reflash.

## Configuration

Most of the behavior, save for the [display specifications]() (which will one day be configurable as well), is configurable at runtime via the Redis instance the unit connects to.

Specifically, a [number of fields](https://github.com/rpj/zw/blob/master/zw_common.h#L7-L13) are exposed as `HOSTNAME:config:*` keys for which any written (valid) value [will be honored](https://github.com/rpj/zw/blob/master/zero_watch.ino#L264-L295) on the next refresh cycle.

There is also a [control point](https://github.com/rpj/zw/blob/master/zero_watch.ino#L134) key at `HOSTNAME:config:controlPoint`, a [metadata getter](https://github.com/rpj/zw/blob/master/zero_watch.ino#L73) at `HOSTNAME:config:getValue` and the [OTA update configuration](https://github.com/rpj/zw/blob/master/zero_watch.ino#L185) key at `HOSTNAME:config:update`.
