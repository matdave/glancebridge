# NOTES.md — session context (rebuild-after-VM-loss)

> Written 2026-09-19 so the project can be resumed if the dev VM is destroyed
> and only this directory survives. Everything below is also in the git
> history; this file is the fast-path summary.

## 1. Current status snapshot

**Working, validated on real hardware (user's Adafruit Feather ESP32 V2,
classic ESP32, clock = `GlanceClock_3B e3:75:8d:6f:22:c8`, bonded):**
- BLE central client: scan → PIN pairing → bonded reconnect (no PIN on reboot)
- `notify <text>` — protobuf Notice displayed on the clock (MVP goal, works)
- Console commands all functional; see README.md table

**Built, committed, but NOT yet hardware-validated:**
- WiFi + NTP (`wifi <ssid> <pass>`, `tz <posix>` → NetTime) → time syncs
- Current Time Service (clock polls it after connecting → hands follow ESP32 time)
- ChronosBridge: phone via Chronos app → time sync (CF_TIME → settimeofday →
  CTS) and notification relay (app: title → Notice). Starts at boot.
- `settime YYYY-MM-DD HH:MM:SS` manual time setting

**Open question (active investigation):**
- `settings` read: the write of command 35 (UpdateAndRefresh, frame
  `23 20 00 00` or `23 00 00 00`) is **rejected by the clock's ATT server
  almost instantly** while `notify` (command 2) writes to the SAME
  characteristic succeed. NOT a NimBLE stall (connection stays up).
- NimBLE error logging was just enabled (`-DCONFIG_NIMBLE_CPP_LOG_LEVEL=1`),
  so the next `settings` attempt prints `writeValue failed, rc: N <desc>`.
  **Get that rc from the user** — it identifies why (2=request not
  supported, 5=insufficient auth, etc.).
- Untested hypothesis: HA integration (PorlyBe/glance_clock_ha) sends
  command 35 as a **single byte** `bytes([35])`. User can test without
  reflash via console: `raw 23` (single byte) vs `raw 23000000` (our frame).
- Undocumented service `8e400001-f315-4f60-9fb8-838830daea50`
  (read+write+notify, CCCD subscribable via `sub on|off`): purpose unknown.
  Suspected response/push channel. `sub on` did NOT make settings respond,
  and did NOT break writes in the one A/B test done.

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
- The web app / HA flow sends command **35 (UpdateAndRefresh) before
  touching settings** ("prepare device for settings changes").
- Reading settings per HA integration: plain read of the Data char;
  skip "Data" (4 bytes) + NUL (1 byte); elif first byte == 5, skip it.

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

1. Ask for the result of: `settings` (NimBLE rc line) and `raw 23` vs
   `raw 23000000` — resolves the command-35 rejection mystery.
2. Have them test (in order):
   - `wifi <ssid> <pass>` + `tz <posix>` → `[NetTime] time synced via NTP`
     → clock hands jump to real time (validates CTS end-to-end)
   - Install Chronos app (https://chronos.ke/app?id=esp32) → pair
     "GlanceBridge" → `status` shows `phone=1`, log shows
     `[bridge] phone time sync: ...` → hands re-sync to phone time
   - Trigger a phone notification → `[bridge] relayed: ...` + clock display
3. If settings still rejected after rc is known: options are (a) treat cmd 35
   as unavailable on this firmware and find another settings-publish trigger
   (maybe via 8e400001 or after specific scene pushes), or (b) skip settings
   read entirely (it is diagnostics-only; write path for settings is
   `[5,0,0,0]+Settings protobuf` per HA integration and may still work).
4. Phase 3 backlog: ForecastScene (24h hourly forecast from Chronos → ring
   display), alarms (Chronos Alarm struct → Alarms protobuf), CallScene from
   ringer callback. All protobufs already generated in lib/GlanceCore.

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
