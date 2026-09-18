# Glance Clock ESP32 Bridge

An ESP32 firmware that acts as a **cloudless bridge** for the
[Glance Clock](https://github.com/Hypfer/glance-clock), using the
[Chronos app](https://chronos.ke/app?id=esp32) +
[chronos-esp32](https://github.com/fbiego/chronos-esp32) as the data source.

```
Phone (Chronos app)                        Glance Clock
  │ notifications, time,                       ▲
  │ weather, alarms, calls                     │ BLE central (NimBLE)
  ▼                                            │ protobuf over GATT
ESP32 (S3/C3) ── chronos-esp32 (peripheral) ───┴── GlanceClient (central)
            both roles on one NimBLE host
```

Protocol source: [Hypfer/glance-clock](https://github.com/Hypfer/glance-clock)
(WTFPL). The clock's BLE protocol is a 4-byte command header plus a protobuf
payload, written to characteristic
`5075fb2e-1e0e-11e7-93ae-92361f002671` of service
`5075f606-1e0e-11e7-93ae-92361f002671`.

## Status / roadmap

- [x] **MVP**: BLE central client, PIN pairing (bonding in NVS), `Notify` command
- [x] Serial console (`scan`, `pair`, `notify`, `stop`, `start`, `clear`, ...)
- [x] Host-side protocol unit tests (native PlatformIO env)
- [x] **Current Time Service**: the clock polls it after connecting and sets
      its hands from the ESP32 system clock
- [x] **ChronosBridge**: pair the phone via the Chronos app; phone time syncs
      the ESP32 (and therefore the clock hands) and phone notifications are
      relayed to the clock
- [x] Settings read/write (command 5) — read requires an UpdateAndRefresh
      first, response is prefixed with `"Data\0"`
- [ ] Weather: Chronos hourly forecast -> `ForecastScene` (24h ring display)
- [ ] Chronos alarms -> `Alarms` protobuf
- [ ] Incoming call -> `CallScene`
- [ ] Undocumented `8e400001-f315-4f60-9fb8-838830daea50` service (read+write+
      notify, subscribable via `sub on`) — purpose unknown, possibly the
      response/push channel the official app uses

## Hardware

Tested boards: `esp32-s3-devkitc-1`, `esp32-c3-devkitm-1` (any ESP32/S3/C3
dev board should work). The clock only needs BLE range.

## Build & flash

```sh
pio run -e esp32s3 -t upload        # or -e esp32c3
pio device monitor                  # 115200 baud
```

## Pairing checklist (first time)

1. `pio device monitor`, then type `scan` — the clock should appear
   (advertises the Glance service, name usually contains "Glance").
2. **Press the pairing button on the clock.**
3. Type `pair`. The clock shows a 6-digit PIN on its LED rings.
4. Type the PIN in the serial monitor and press Enter.
5. The clock plays an animation; the ESP32 stores the address in NVS.
   After a reboot it reconnects silently (bonded).
6. Test with `notify hello`.

If pairing fails: `bonds` (clears pairings *in the clock*), `forget` (clears
the stored address), and if needed
[factory reset the clock](https://github.com/Hypfer/glance-clock#factory-reset).

## Console commands

| Command | Action |
| --- | --- |
| `scan [ms]` | scan for Glance clocks |
| `pair` | connect + PIN pairing (press clock button first) |
| `notify <text>` | show a notification on the clock |
| `stop` / `start` | previous / next scene slot |
| `clear` | clear all scenes |
| `bonds` | clear pairings stored in the clock |
| `refresh` | UpdateAndRefresh (cloud-update animation) |
| `night on\|off` | automatic night mode |
| `gatt` | dump the clock's GATT table (names + values) |
| `sub on\|off` | (un)subscribe to the undocumented 8e400001 channel |
| `settings` | re-request the clock's settings |
| `time` / `settime` | show / set the ESP32's local time (`settime YYYY-MM-DD HH:MM:SS`) |
| `chronos on\|off` | start the Chronos peripheral / pause the notification relay |
| `forget` | forget the stored clock address |
| `status` | connection, relay, phone and settings state |
| `raw <hex>` | write raw bytes to the data characteristic |

## Layout

```
proto/                upstream Glance.proto (patched, see header comment) + nanopb options
lib/GlanceCore/       protocol layer: command framing + nanopb-generated messages
src/glance/           GlanceClient (NimBLE central: scan, pair, send commands)
src/Console.h         serial command interface
test/test_protocol/   host-side protocol tests (pio test -e native)
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
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform fork.
- Both BLE roles (central to the clock, later a peripheral for the phone) run
  on one NimBLE host; add the Chronos side incrementally.

## License

Project code: WTFPL (matches the protocol repo). Dependencies keep their own
licenses (chronos-esp32 & NimBLE-Arduino: MIT, Nanopb: Zlib).
