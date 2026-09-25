# Glance Clock ESP32 Bridge

Firmware for an ESP32 that connects your [Glance Clock](https://github.com/Hypfer/glance-clock)
to your phone without any cloud service. The clock talks to the ESP32 over
Bluetooth Low Energy, the phone connects to the ESP32 with the
[Chronos app](https://chronos.ke/app?id=esp32), and the two are joined
locally:

```
Phone (Chronos app)                          Glance Clock
  | notifications, time,                        ^
  | battery                                     | BLE (NimBLE)
  v                                             | protobuf over GATT
ESP32 -- chronos-esp32 (peripheral) ------------+-- GlanceClient (central)
  |  both roles on one NimBLE host
  |- WiFi + NTP (authoritative time) -- Current Time Service for the clock
  |- Web portal  http://<name>.local   status / PIN / weather / controls
  '- Open-Meteo weather -- 24h forecast ring on the clock face
```

Everything runs on your LAN. No accounts, no subscriptions, no cloud.

## What it does

- Pairs with the clock once, then reconnects silently on reboot
- Shows phone notifications on the clock face
- Pushes the 24h temperature forecast (from Open-Meteo, no API key) as a
  ring on the clock's carousel
- Keeps the clock's hands on time: NTP first, then the phone's time, or a
  manual `settime`
- Syncs alarms and battery level with the Chronos app
- Lets you control the clock from a web page or a serial console:
  brightness, night mode, hands calibration, carousel navigation

## Requirements

- An ESP32 (the Adafruit Feather ESP32 V2 is the primary target; `esp32s3`
  and `esp32c3` also build)
- A Glance Clock (clock firmware 1.5 or newer for the forecast ring)
- The [Chronos app](https://chronos.ke/app?id=esp32) on your phone
  (optional; notifications, alarms and time push need it)
- WiFi (optional; NTP time + the web portal need it)

## Build and flash

Install [PlatformIO Core](https://platformio.org/install/cli), then:

```sh
pio run -e adafruit_feather_esp32_v2 -t upload   # or -e esp32s3 / -e esp32c3
pio device monitor                               # 115200 baud serial console
```

The serial console is the main way to talk to the bridge. Type `help` for
the command list.

## First-time setup

1. **Connect to WiFi** (needed for NTP time and the web portal):
   ```
   wifi <your-network> <password>
   tz EST5EDT,M3.2.0,M11.1.0        # or your local POSIX timezone
   ```
   Wait for `[NetTime] time synced via NTP` — the clock hands will follow.

2. **Pair the clock** (one time):
   ```
   scan
   ```
   Press the pairing button on the clock, then:
   ```
   pair
   ```
   The clock shows a 6-digit PIN on its LED rings. Type it in the serial
   console (or the web portal — see below). You have about 25 seconds per
   attempt, and the clock shows a fresh PIN on each retry. After pairing,
   the bridge reconnects on its own from then on.

3. **Optional: set the forecast location** so the ring appears:
   ```
   geo <latitude> <longitude>   # decimal degrees, e.g. geo 40.586 -98.389
   wx c                         # or wx f for Celsius/Fahrenheit (default F)
   ```

## Web portal

Once WiFi is up, open `http://glancebridge.local` in a browser (the name
is configurable per device with `mdns <name>`). All pages are self-contained.

| Page | What it's for |
| --- | --- |
| `/` | live status: clock, battery, WiFi, NTP, phone, weather |
| `/enter` | PIN entry during pairing (opens automatically when a PIN is requested) |
| `/weather` | set location and units, fetch the forecast now |
| `/control` | carousel prev/next, hands calibration, clear scenes |

## Serial console commands

| Command | What it does |
| --- | --- |
| `scan [ms]` | scan for Glance clocks |
| `pair` | connect and pair (press the clock's pairing button first) |
| `notify <text>` | show a notification on the clock |
| `stop` / `start` | carousel: previous / next face |
| `clear` | clear all scenes |
| `bonds` | clear pairings stored in the clock |
| `night on\|off` | night mode (settings write) |
| `calib` / `calok` | start / confirm hands calibration |
| `face [slot] [mode]` | custom scene probe (mode 8 = watchface) |
| `gatt` | dump the clock's GATT table |
| `settings` | read the clock's published settings |
| `cfg [0-255]` | write settings (optional brightness) |
| `batt` | read the clock's battery level |
| `geo <lat> <lon>` | store the forecast location |
| `wx [c\|f]` | fetch and show the 24h forecast |
| `time` / `settime ...` | show / set the ESP32's local time |
| `chronos on\|off` | start the Chronos peripheral / pause the relay |
| `alarms` / `alarmpush` | list / push the Chronos alarms to the clock |
| `call <name>` | fake an incoming call notice (test) |
| `wifi <ssid> <pass>` | connect to WiFi and sync time via NTP |
| `wifioff` | clear stored WiFi credentials |
| `tz [posix]` | show / set the timezone |
| `forget` | forget the stored clock address |
| `disc` | clean disconnect (diagnostic) |
| `mdns [name]` | show / set the web portal host name |
| `status` | connection, clock, WiFi, NTP, phone, battery state |
| `help` | this list |

## Project layout

```
proto/                upstream Glance.proto + nanopb options
lib/GlanceCore/       protocol layer: command framing + generated messages
src/glance/           GlanceClient: BLE central (scan, pair, commands, battery)
src/bridge/           ChronosBridge: peripheral for the phone (time, notifications)
src/net/              NetTime (WiFi + NTP), Forecast (weather ring), WebPortal
src/Console.h         serial command interface
boards/               custom Feather ESP32 V2 board definition
```

The generated protobuf code in `lib/GlanceCore/src/` is committed, so a
normal build needs no protoc. To regenerate it after editing `proto/Glance.proto`:

```sh
pip install nanopb
cd proto
nanopb_generator -f glance.options -D ../lib/GlanceCore/src Glance.proto
```

## Troubleshooting

- **Pairing fails repeatedly** — `bonds` clears pairings in the clock,
  `forget` clears the stored address, then try `pair` again.
- **The clock demands a PIN after a reflash** — clock firmware 1.5 drops
  the bond when the bridge disconnects abnormally (unplug / reflash). Just
  enter the PIN again; the bond is re-established.
- **The forecast ring does not appear** — the clock firmware must be 1.5+;
  older factory firmware rejects scene commands.
- **WiFi drops but the bridge keeps running** — the bridge now watches the
  link and forces a clean reconnect automatically (look for
  `forcing clean reconnect` in the serial log).

## Credits

This project builds on the work of several projects:

- [Hypfer/glance-clock](https://github.com/Hypfer/glance-clock) — the
  Glance Clock itself and its BLE protocol (the clock side this firmware
  talks to). The protocol definition lives here, and this project's code
  is WTFPL to match.
- [frannraf/glance-clock-control](https://github.com/frannraf/glance-clock-control)
  — the C# reference client whose validated frame layouts were used to
  cross-check command and scene formats.
- [fbiego/chronos-esp32](https://github.com/fbiego/chronos-esp32) — the
  Chronos BLE peripheral library (time, notifications, alarms, battery)
  used to connect the phone app to the bridge.
- [h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — the
  BLE stack that runs both the central (clock) and peripheral (phone) roles.
- [Nanopb](https://github.com/nanopb/nanopb) — the protobuf code generator
  used for the Glance protocol messages.
- [Open-Meteo](https://open-meteo.com) — the free weather API behind the
  forecast ring (no API key required).

## License

Project code: WTFPL (matches the protocol repo). Dependencies keep their
own licenses (chronos-esp32 & NimBLE-Arduino: MIT, Nanopb: Zlib).