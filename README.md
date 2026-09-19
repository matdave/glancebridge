# Glance Clock ESP32 Bridge

An ESP32 firmware that acts as a **cloudless bridge** for the
[Glance Clock](https://github.com/Hypfer/glance-clock), using the
[Chronos app](https://chronos.ke/app?id=esp32) +
[chronos-esp32](https://github.com/fbiego/chronos-esp32) as the data source.

```
Phone (Chronos app)                         Glance Clock
  │ notifications, time,                        ▲
  │ battery                                     │ BLE central (NimBLE)
  ▼                                             │ protobuf over GATT
ESP32 ── chronos-esp32 (peripheral) ────────────┴── GlanceClient (central)
  │  both roles on one NimBLE host
  ├─ WiFi + NTP (authoritative time) ── Current Time Service for the clock
  └─ Web portal  http://<name>.local   status / PIN / weather / controls
```

Protocol source: [Hypfer/glance-clock](https://github.com/Hypfer/glance-clock)
(WTFPL). The clock's BLE protocol is a 4-byte command header plus a protobuf
payload, written to characteristic
`5075fb2e-1e0e-11e7-93ae-92361f002671` of service
`5075f606-1e0e-11e7-93ae-92361f002671`. Some commands are single-byte
(35, 30, 31, 43, 44, 60, 61); sending those as 4-byte frames is rejected with
a vendor ATT error (0x81).

**Clock firmware:** scenes/forecast (`cmd 7`) require firmware 1.5+ — factory
0.8.56 rejects them. DFU zips per hardware revision are in
[Hypfer/glance-clock-assets/firmwares](https://github.com/Hypfer/glance-clock-assets/tree/master/firmwares)
(the zip name starts with the hw rev, e.g. `0503...` = hw 5.3); flash via
nRF Connect DFU. After a DFU, re-pair once (the update wipes the clock's
bond store).

## Status / roadmap

- [x] **MVP**: BLE central client, PIN pairing (bonding in NVS), `Notify` command
- [x] Serial console (see command table below) with terminal line editing
- [x] Host-side protocol unit tests (native PlatformIO env, 9 cases)
- [x] **Current Time Service**: the clock polls it after connecting; the
      ESP32 re-connects the clock whenever the system time changes so the
      hands follow immediately
- [x] **Time sources, in priority order**: NTP (authoritative), Chronos
      phone time (fallback), `settime`
- [x] **ChronosBridge**: phone time syncs the ESP32, phone notifications are
      relayed to the clock, clock battery is pushed to the app
- [x] Settings read/write (command 5): read-modify-write; the clock publishes
      the new settings back into the data characteristic after a write
- [x] **Forecast ring**: 24h temperatures from Open-Meteo (no API key),
      `ForecastScene` in carousel slot 1; shows 15 s then auto-deletes so the
      clock returns to its native watchface; auto-refresh every 30 min
- [x] **Web portal**: status, headless PIN entry, weather config, carousel
      and calibration controls (see below)
- [ ] Chronos alarms -> `Alarms` protobuf
- [ ] Incoming call -> `CallScene`
- [ ] Other weather rings (rain / wind / humidity — frame builders exist in
      the C# reference client)
- [ ] Undocumented `8e400001-f315-4f60-9fb8-838830daea50` service (read+write+
      notify, subscribable via `sub on`) — purpose unknown

## Hardware

Primary: Adafruit Feather ESP32 V2 (classic ESP32) via the custom board def
`boards/glance_feather_esp32_v2.json` — do not delete it: the stock Feather
definition overflows IRAM once WiFi + NimBLE are linked, and the app
partition of the stock layout is too small. `esp32s3` / `esp32c3` envs build
too (C3 is built in CI).

## Build & flash

```sh
pio run -e adafruit_feather_esp32_v2 -t upload   # or -e esp32s3 / -e esp32c3
pio device monitor                               # 115200 baud
pio test -e native                               # host-side protocol tests
```

## Pairing

1. `pio device monitor`, then `scan` — the clock should appear.
2. **Press the pairing button on the clock.**
3. Type `pair`. The clock shows a 6-digit PIN on its LED rings.
4. Enter the PIN **either in the serial monitor or on the web portal**
   (`http://glancebridge.local` — the form appears automatically while the
   PIN prompt is active). You have ~25 s per attempt; the clock shows a
   fresh PIN on each retry.
5. After a successful pair the ESP32 stores the address in NVS and
   reconnects silently.

Bonding behavior of clock firmware 1.5: the clock **keeps** its bond across
its own power cycle, but **deletes it when the ESP32 disconnects abnormally**
(unplug/reflash = supervision timeout). The next connection then requires a
PIN re-entry — enter it on the web portal or serial and the bond is
re-established. This is clock-side behavior and cannot be disabled.

If pairing fails: `bonds` (clears pairings *in the clock*), `forget` (clears
the stored address), and if needed
[factory reset the clock](https://github.com/Hypfer/glance-clock#factory-reset).

## Web portal

Served on the LAN once WiFi is up: `http://glancebridge.local` (name
configurable per device via `mdns <name>`, stored in NVS — useful with
several bridges). All pages are self-contained (no external assets).

| Page | Purpose |
| --- | --- |
| `/` | live status (clock/battery/WiFi/NTP/phone/weather), auto-refresh |
| `/enter` | PIN form (headless pairing; opens automatically when a PIN is requested) |
| `/weather` | location (lat/lon), °C/°F, fetch now |
| `/control` | carousel prev/next, hands calibration, clear scenes |

## Console commands

| Command | Action |
| --- | --- |
| `scan [ms]` | scan for Glance clocks |
| `pair` | connect + PIN pairing (press clock button first) |
| `notify <text>` | show a notification on the clock |
| `stop` / `start` | carousel: previous / next face (single-byte 30/31) |
| `clear` | clear all scenes |
| `bonds` | clear pairings stored in the clock |
| `night on\|off` | night mode (settings write) |
| `calib` / `calok` | start / confirm hands calibration |
| `face` | (re)install the digital watchface as carousel slot 0 |
| `gatt` | dump the clock's GATT table (names + values) |
| `sub on\|off` | (un)subscribe to the undocumented 8e400001 channel |
| `settings` | read the clock's published settings (best effort) |
| `cfg [0-255]` | read-modify-write settings; optional brightness |
| `batt` | read the clock's battery level (also cached for `status`) |
| `geo <lat> <lon>` | store the forecast location (decimal degrees) |
| `wx [c\|f]` | fetch + show the 24h forecast (unit persisted, default F) |
| `time` / `settime` | show / set the ESP32's local time (`settime YYYY-MM-DD HH:MM:SS`) |
| `chronos on\|off` | start the Chronos peripheral / pause the notification relay |
| `wifi <ssid> <pass>` | connect to WiFi and sync time via NTP |
| `wifioff` | clear stored WiFi credentials |
| `tz [posix]` | show/set timezone, e.g. `tz EST5EDT,M3.2.0,M11.1.0` |
| `forget` | forget the stored clock address |
| `disc` | clean disconnect (bond-keep diagnostic) |
| `mdns [name]` | show/set the web portal host name |
| `status` | clock, WiFi, NTP, phone, battery and settings state |
| `raw <hex>` | write raw bytes to the data characteristic |
| `raws <hex>` | write raw bytes to the scene characteristic |

## Layout

```
proto/                upstream Glance.proto (patched, see header comment) + nanopb options
lib/GlanceCore/       protocol layer: command framing + nanopb-generated messages,
                      Notice/Settings/ForecastScene encoders + decoders
src/glance/           GlanceClient (NimBLE central: scan, pair, commands, battery, CTS nudge)
src/bridge/           ChronosBridge (peripheral for the phone: time, notifications, battery)
src/net/              NetTime (WiFi+SNTP), Forecast (Open-Meteo ring), WebPortal
src/Console.h         serial command interface
test/test_protocol/   host-side protocol tests (pio test -e native)
boards/               custom Feather ESP32 V2 board definition (no PSRAM workaround)
```

### Regenerating protobuf code

The generated `lib/GlanceCore/src/Glance.pb.[ch]` are committed, so you don't
need protoc to build. To regenerate after changing `proto/Glance.proto`:

```sh
pip install nanopb            # + grpcio-tools, or a protoc binary
cd proto
nanopb_generator -f glance.options -D ../lib/GlanceCore/src Glance.proto
```

`glance.options` gives every variable-length field a static size, so the
generated code never needs malloc.

## Notes

- `NimBLE-Arduino` 2.x needs arduino-esp32 3.x -> this project uses the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform
  fork, pinned at **54.03.20** (newer versions made IRAM usage worse).
- Both BLE roles (central to the clock, peripheral for the phone) run on one
  NimBLE host.
- **Local dependency patches** (re-apply after a library reinstall):
  - `ESP32Time.cpp` (`setTime(sc, mn, hr, ...)`): `struct tm` must use
    `tm_isdst = -1` — the upstream `0` forces standard time, skewing phone
    time pushes by +1 h during DST.
  - The web PIN portal assumes `timeformat=unixtime` from Open-Meteo, which
    returns localtime-as-epoch — exactly what the `ForecastScene.timestamp`
    field expects.
- Known firmware quirks (clock 1.5): bond dropped on abnormal central
  disconnect (see Pairing), rare BLE controller assert (`llc.c`) when the
  phone and clock connections are established simultaneously (recovers by
  rebooting; the first clock reconnect is staggered 5 s at boot to reduce
  the chance).

## License

Project code: WTFPL (matches the protocol repo). Dependencies keep their own
licenses (chronos-esp32 & NimBLE-Arduino: MIT, Nanopb: Zlib).
