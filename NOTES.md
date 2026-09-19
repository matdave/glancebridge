# NOTES.md — session context (rebuild-after-VM-loss)

> Written 2026-09-19 so the project can be resumed if the dev VM is destroyed
> and only this directory survives. Everything below is also in the git
> history; this file is the fast-path summary.

## 1. Current status snapshot

**Working, validated on real hardware (user's Adafruit Feather ESP32 V2,
classic ESP32, clock = `GlanceClock_3B e3:75:8d:6f:22:c8`, bonded):**
- BLE central client: scan → PIN pairing → bonded reconnect (no PIN on reboot)
- `notify <text>` — protobuf Notice displayed on the clock
- Console commands all functional (see README/help); line editing works
- Settings: read-modify-write + read-after-write echo (validated)
- `batt` (0x180F), `calib`/`calok` (43/44), `stop`/`start` (30/31) — validated
- WiFi + NTP (`wifi`, `tz` → NetTime) — validated incl. runtime re-connect
- CTS push nudge on time change (NTP first sync / settime / phone sync →
  reconnect → clock re-polls) — built; hands-follow-time confirmed working
- ChronosBridge: phone time sync + notification relay — built, starts at
  boot; status shows phone=0 until the Chronos app test is actually run

**Built, committed, but NOT yet hardware-validated:**
- WiFi + NTP (`wifi <ssid> <pass>`, `tz <posix>` → NetTime) → time syncs
- Current Time Service (clock polls it after connecting → hands follow ESP32 time)
- **CTS push nudge (built, untested)**: when system time first becomes valid
  (NTP) — and on `settime` and phone time sync — the clock is disconnected
  and immediately reconnected (`GlanceClient::refreshClockTime()`), forcing
  it to re-poll CTS right away instead of waiting for its own schedule.
  Chronos note: ChronosESP32 inherits ESP32Time and applies the phone time
  via settimeofday BEFORE our CF_TIME callback fires.
- ChronosBridge: phone via Chronos app → time sync (CF_TIME → settimeofday →
  CTS) and notification relay (app: title → Notice). Starts at boot.
- `settime YYYY-MM-DD HH:MM:SS` manual time setting
- WiFi + NTP (`wifi <ssid> <pass>`, `tz <posix>` → NetTime) → time syncs
  **(VALIDATED 2026-09-19: boot connect + runtime `wifioff`→`wifi` both work;
  the runtime failure was WiFi/BLE coex — fixed with `WiFi.setSleep(false)`
  + clean `disconnect()` + 100ms before `begin()`; heap ~105KB is fine)**
- `batt` — clock battery via standard 0x180F/0x2A19 (**VALIDATED: 100%**;
  also cached for `status`)
- Console now handles terminal line-editing keys (BS/DEL erase, ANSI ESC
  sequences swallowed — arrow/history keys used to leak `1b ... 08` bytes
  into the line, making valid commands report "unknown"; raw-bytes debug
  print on unknown commands left in for now)
- **Console cleanup 2026-09-19 (hardware-VALIDATED — "everything seems to be
  working")**: removed `refresh` (cmd 35 no-op on cloud-less clocks).
  `stop`/`start` now single-byte 30/31 (the 4-byte frames were likely
  0x81-rejected like 35). `night on|off` is now a settings WRITE (40/41
  command frames are from the cloud era) using a read-modify-write base;
  `cfg` also now bases on last decoded settings
  (`GlanceClient::lastSettings()`, populated by successful `settings`
  reads) with `completeSettings()` setting every has_* flag and preserving
  dnd/silent schedules when known. NEW: `calib` (single-byte 43, starts
  hands calibration) + `calok` (single-byte 44, confirm) — both validated.
  Single-byte commands 10/30/31/35/43/44/60/61 are the confirmed pattern.

**Settings: SOLVED and hardware-validated 2026-09-19:**
- **Write path works**: `cfg 200` / `cfg 128` → `[5,0,0,0] + Settings proto`
  (24 bytes) → accepted, clock applies values (brightness 200↔128 verified
  via read-back).
- **Publish trigger found**: after processing a settings write, the clock
  publishes the settings into the data characteristic's readable value.
  `settings` then reads + decodes them (e.g. `10 01 18 00 20 00 28 00 48 00
  50 C8 01 58 01 60 00 68 D8 04` = night=1 dnd=0 mute=0 date=0 points=0
  bright=200 timeMode=1 12h=0 activity=600). A plain read is otherwise
  usually empty — read-after-write is the reliable pattern.
- cmd 35 (UpdateAndRefresh) is useless here: rejected with vendor ATT 0x81
  in both 4-byte and single-byte form (clock is cloud-less; cmd 35 = "pull
  from Glance cloud"). No-response writes go out but publish nothing.
  Leave as-is (sendCommand's no-response fallback is harmless + HA-proven).
- Undocumented service `8e400001-f315-4f60-9fb8-838830daea50`
  (read+write+notify, CCCD subscribable via `sub on|off`): purpose unknown.
  Suspected response/push channel. Subscribing did not affect anything.

## 2. Protocol knowledge (learned on hardware — preserve!)

Full GATT table dump (see `gatt` command) — Glance service
`5075f606-...` contains:
- `5075fb2e-...` **'Data'** — read+write, **NO notify**. Command channel:
  write `[cmdType, prio, a, b] + protobuf`. Value read returns either empty,
  or `"Data\0" + protobuf payload` (strip 5 bytes).
- `5075ffac-...` **'Scene'** — write-only (probably scene streaming)
- `5075fc78-...` **'State'** — read-only, 1 byte (was 0x00, never changed)
- `8e400001-...` — read+write+notify with CCCD (see open question)
- Also standard services: 0x1800, 0x1801, 0x180a (device info), 0x180f
  (battery, read+notify).
- Settings message from clock (when it publishes) is prefixed `"Data\0"`;
  per glance_clock_ha also sometimes a raw frame with first byte == 0x05.
- Reading settings per HA integration / working C# client: plain read of the
  Data char; strip `"Data\0"` (4 bytes text + NUL), or `[5,0,0,0]` (4 bytes),
  or a single byte == 5; then protobuf.
- **Settings write (VALIDATED on hardware 2026-09-19)**: `[5,0,0,0] +
  Settings protobuf`, sent with-response, replaces ALL fields. After
  processing, the clock echoes the settings back into the Data read value
  (any of the three envelopes) — read-after-write is the reliable pattern.
  Field map confirmed live: 2=nightMode, 3=permDND, 4=permMute, 5=dateFormat,
  9=points, 10=brightness (varint, e.g. `50 C8 01` = 200), 11=timeMode,
  12=12h, 13=activityTimeout (`68 D8 04` = 600).
- **Single-byte commands are a real firmware pattern**: 35 (UpdateAndRefresh,
  HA), 60/61 (brightness scene stop/start, HA), 10 (TimerStop), 30/31
  (scene nav, direction unverified, C#). 4-byte frames for 35 are rejected
  (ATT 0x81); send single-byte commands as one byte only.
- HA forecast command header: `[7, 16, 24, 1]` (cmd 7, priority 16, 24
  hours, slot 1) + ForecastScene protobuf (timestamp, max, min, maxColor,
  minColor, values=24x Int16LE, template bytes).
- Reference client with validated frame layouts (timers, rings/forecast,
  scene slots, text modifiers `[icon:N]`, weather→animation/color maps):
  frannraf/glance-clock-control `GlanceProtocol.cs` — cross-check there
  before guessing protocol details.

## 3. Environment rebuild (VM loss recovery)

Toolchain install order (only what's needed; 7.9GB disk fills fast!):

```sh
pip3 install --user --break-system-packages platformio
# nanopb codegen only needed when regenerating proto (generated files are
# committed): 
pip3 install --user --break-system-packages nanopb grpcio-tools
mkdir -p ~/.local/bin && printf '#!/bin/sh\nexec python3 -m grpc_tools.protoc "$@"\n' \
  > ~/.local/bin/protoc && chmod +x ~/.local/bin/protoc
export PATH="$HOME/.local/bin:$PATH"
pio run -e adafruit_feather_esp32_v2   # downloads platform + xtensa toolchain + libs (~4.5GB)
pio test -e native                     # 7 protocol tests must pass
```

**Disk discipline (7.9GB VM disk — it WILL fill up):**
- `~/.platformio/.cache` grows with every package install; `rm -rf` it freely.
- `~/.platformio/packages/toolchain-riscv32-esp` is 2.4GB and only needed for
  the `esp32c3` env. Delete it locally; CI (.github/workflows/build.yml)
  builds C3 in the cloud. pio may re-install it on the next feather build —
  if disk explodes again, delete + retry immediately.
- `framework-arduinoespressif32-libs` (1.9GB) + xtensa toolchain (~1.2GB) are
  required; don't delete both at once while a build is needed.
- Build failures of two kinds seen: `Errno 28 No space left` during tool
  install (fix: free space, delete the half-installed package dir, retry) —
  and root-owned files in `.pio/` when another process (user's IDE, running
  as root) built in the same workspace concurrently (fix: `sudo rm -rf
  .pio/build .pio/libdeps/<env>` then rebuild as the normal user).

**Known noise, non-fatal:** `error: externally-managed-environment` printed
by the espressif framework's "Installing Arduino Python dependencies" step;
the build continues and succeeds after it.

**Platform pin (do not bump casually):** pioarduino **54.03.20** — tried
55.03.39, IRAM overflow got WORSE (1872B vs 1072B). NimBLE 2.5.1 + pioarduino
54.03.20 is the validated combo.

**Custom board (critical!):** `boards/glance_feather_esp32_v2.json` — copy of
adafruit_feather_esp32_v2 WITHOUT `-DBOARD_HAS_PSRAM` and without
`-mfix-esp32-psram-cache-*` codegen flags. The stock Feather board def
overflows `iram0_0_seg` by ~1KB once WiFi + NimBLE are linked (the workaround
codegen inflates all source-compiled code: Arduino core + NimBLE). The bridge
never touches PSRAM, so the workaround is pure loss. Also set
`board_build.partitions = default_8MB.csv` (stock layout leaves the app
partition at 1.31MB; we were at 97% full). **Do not delete this file.**

## 4. Next steps when user returns

1. Settings: DONE (write + read-after-write validated). `batt` validated.
   WiFi regression RESOLVED (coex; see section 1). Diagnostics live in
   NetTime (status transitions) and Console (raw line bytes on unknown).
2. Validate (in order):
   - `wifi <ssid> <pass>` + `tz <posix>` → `[NetTime] time synced via NTP`
     → clock hands jump to real time (validates CTS end-to-end)
   - Install Chronos app (https://chronos.ke/app?id=esp32) → pair
     "GlanceBridge" → `status` shows `phone=1`, log shows
     `[bridge] phone time sync: ...` → hands re-sync to phone time
   - Trigger a phone notification → `[bridge] relayed: ...` + clock display
3. Phase 3 backlog: ForecastScene (24h hourly forecast from Chronos → ring
   display), alarms (Chronos Alarm struct → Alarms protobuf), CallScene from
   ringer callback. All protobufs already generated in lib/GlanceCore. The
   C# GlanceProtocol.cs has validated ring/forecast frame builders to port.

## 5. Handy facts

- NimBLE callbacks run on the host task: NEVER call blocking BLE ops
  (secureConnection, readValue, writeValue) inside them. Pairing flow does
  everything in main task after connect() returns (see GlanceClient).
- `writeValue` with response blocks until ACK or the host's hard-coded 30s
  GATT timeout (`BLE_GATTC_UNRESPONSIVE_TIMEOUT_MS`), which then drops the
  connection. Every sendCommand logs `sending cmd 0xNN` first for visibility.
- NimBLE 2.x discovery is lazy: connect() does NOT populate the service
  cache; `discoverAttributes()` must be called for a full dump; getService()
  does its own filtered discovery.
- `NimBLEDevice::init()` is idempotent (safe when ChronosESP32::begin()
  calls it again); both roles share the NimBLE server singleton.
- Serial console: pio device monitor does NOT echo typed chars and sends
  `\r` on Enter — Console now echoes + handles CR/LF/CRLF + shows `> `
  prompt. PIN entry drains stale input and validates digits (typed-ahead
  keystrokes once submitted `000000` and failed pairing).
- `getLocalTime()` returns false until system time > 2016 (epoch boot).
  CTS keeps its old value until first valid refresh.
- User's machine builds from `/home/matdave/GIT/python/glanceclock` (they
  sync the repo there themselves); default env is their Feather board.
- All native tests: `pio test -e native` → 7 pass. Reference frame test
  (`02 30 00 00 22 03 12 01 41`) is byte-exact from the protocol docs.
