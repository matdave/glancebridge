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
- **Forecast ring (VALIDATED after DFU)**: `geo <lat> <lon>` (NVS "weather"
  ns) + `wx [c|f]`; class `Forecast` (renamed from Weather — ChronosESP32.h
  has struct Weather). Open-Meteo HTTPS (setInsecure, ArduinoJson),
  24×hourly temps from the current local hour, frame `[7,16,24,1]` +
  ForecastScene via `Glance::encodeForecastCommand` (native-tested).
  Timestamp = API's unixtime value directly (timeformat=unixtime +
  timezone=auto returns localtime-as-epoch, exactly what the proto wants).
  Auto-refetch every 30 min. **cmd 7 accepted on firmware 1.5** (was 0x81
  on factory 0.8.56 — **and a CLOCK FACTORY RESET ROLLS THE FIRMWARE BACK
  to 0.8.56 (verified 2026-09-19: after the user's reset, scenes were
  0x81-rejected again and gatt showed 0x2a26="0.8.56"; the 1.5.10 DFU had
  to be re-flashed). Avoid factory resets; prefer re-pairing.** Unit
  persisted (default F); auto-hide after 15 s →
  single-byte ScenesStart (31, "next face") advances past the ring back to
  the native watchface. **FACTORY CAROUSEL LAYOUT (clock fw 1.5, user
  discovery 2026-09-19): slot 0 = empty (renders dim), slot 1 = the
  built-in digital watchface!** The community slot scheme (temp=slot 1)
  assumes they installed their own digital-time scene at slot 0 first —
  on this clock forecast to slot 1 OVERWROTE the factory watchface, and
  deleting slot 1 removed it entirely (the "dim" mystery). Forecast now
  goes to SLOT 2. BUG FIXED: the slot byte was HARDCODED as 1 inside
  GlanceCore::encodeForecastCommand (the FORECAST_SLOT constant only fed
  the delete frame + a stale log string) - the first slot-2 build still
  wrote slot 1 and got 0x81-rejected once the user's clock reset restored
  the protected factory watchface. The encoder now takes the slot as a
  parameter (default 2, native test updated). **FORECAST CLOSED
  (user-validated 2026-09-20): slot 2 works as expected with auto-hide
  DISABLED (FORECAST_DISPLAY_MS = 0) — the ring stays in the carousel and
  the factory watchface is untouched. The auto-hide delete code path is
  retained for a future timed-removal option. User feedback drove the
  design: 30/31 are carousel-nav only (C# advances its weather carousel
  with 31), empty
  slots and the mode-8 watchface CustomScene ([0,0,8,slot], C#
  DigitalTimeSceneCommand, byte-identical frame) render BLANK on firmware
  1.5 - likely a 1.6+ feature; `face [slot] [mode]` now takes args for
  probing other display modes. So no auto slot-0
  install (manual `face` command kept for experiments).
- ChronosBridge: phone time sync + notification relay — built, starts at
  boot; status shows phone=0 until the Chronos app test is actually run
- **KNOWN TRANSIENT — BLE controller crash**: `ASSERT_PARAM(0 0) in llc.c
  / r_llc_start` (Guru Meditation, Core 0) when the Chronos phone
  connection and the clock connection are established simultaneously
  (classic-ESP32 controller race in `r_llm_con_req_tx_cfm`). Recovers on
  reboot; recurred twice when phone+clock connected together. Mitigation
  applied: boot stagger — first clock reconnect delayed 5s so the phone
  link settles first.
- **Alarms + calls (BUILT, untested — Phase 3)**: cmd 4, `[4,0,0,0]` +
  Alarms proto via `Glance::encodeAlarmCommand` (native-tested 10/10).
  ChronosBridge: `pushAlarms()` (auto-runs on CF_ALARM config changes),
  `printAlarms()`; repeat-bitfield→Days mapping (Chronos bit0=Mon..bit6=Sun,
  0x7F→All, 0x80/0→None, multi-day→All approximation). Console: `alarms`,
  `alarmpush`. ALARM PRIO IS A GUESS (0) — user-validated anyway: alarms
  DISPLAY correctly on 1.5.10. Originally silent: we sent
  Sound_NoneSound (the literal mute) — fixed to Sound_Waves (proto
  comment: "Also known as Alarm", the classic alarm tone); per-alarm
  sound choice is a possible future knob.
  Incoming call: Chronos ringer callback → Notice with the phone icon
  (raw byte 129) + caller name — there is NO CallScene payload in the
  proto (cmd 6 remains an undocumented experiment). Console `call <name>`
  fakes one. The notice self-dismisses; nothing on call end.
- **CLOCK drops its bond when the central disconnects abnormally**
  (firmware 1.5): ESP32-side bond persists (boot log
  `ESP32 bond for clock: present`), the clock KEEPS the bond across its own
  power cycle, but any ESP32 unplug/reflash (supervision timeout on the
  clock) → clock deletes the bond → next connect demands a PIN. Self-
  healing: the reconnect-PIN flow prompts for the PIN and re-pairs both
  sides (onPassKeyEntry explains it outside pair()). `disc` console
  command = clean disconnect diagnostic: if a clean disconnect+reconnect
  does NOT demand a PIN, the trigger is specifically the abnormal link
  loss. Unfixable from our side (clock security policy requires the PIN
  for re-pairing). writeSettings now auto-reads back after a write
  (3x300ms poll of the published settings) so _lastSettings — and every
  UI built on it — reflects the real clock state without a manual
  'settings' read. **Web portal (src/net/WebPortal.h, renamed from
  PinPortal)** at http://glancebridge.local (mDNS) / device IP:
  / status (auto-refresh), /enter PIN form (NO refresh so typing is safe;
  the pending redirect MUST use meta content='0; url=/enter' - an unquoted
  "content=0 URL=..." parses as reload-every-0s and hammer-refreshes),
  /weather (geo lat/lon + C/F unit + fetch-now), /control (carousel
  prev/next, calib/calok, clear scenes; GET ops redirect so refresh can't
  re-trigger). The mDNS host is configurable per device
  (`mdns <name>` serial command, NVS "portal"/"host", validated
  letters/digits/hyphens, default glancebridge) so multiple bridges can
  coexist on one LAN. When a PIN is requested the console provider marks it
  pending and its 25s wait loop services HTTP (the main loop is blocked
  by the wait). Serial console still works in parallel; missed PIN rounds
  are cheap (the clock shows a fresh PIN on each retry).

**Built, committed, but NOT yet hardware-validated:**
- WiFi + NTP (`wifi <ssid> <pass>`, `tz <posix>` → NetTime) → time syncs
- Current Time Service (clock polls it after connecting → hands follow ESP32 time)
- **CTS push nudge (VALIDATED with a fix)**: when system time first becomes
  valid (NTP) — and on `settime` and phone time sync — the clock is
  disconnected and reconnected (`GlanceClient::refreshClockTime()`), forcing
  it to re-poll CTS right away. Nudges are debounced (3s window — the
  Chronos app sends CF_TIME twice) and the reconnect waits 1500ms for the
  GAP disconnect to settle (immediate reconnect gets refused: "Client not
  disconnected, cannot connect" + rc=7 discovery failures, reason 0x0216;
  500ms occasionally failed with "Connection failed; status=574"
  = BLE_ERR_CONN_ESTABLISHMENT — clock not ready yet).
  Chronos note: ChronosESP32 inherits ESP32Time and applies the phone time
  via settimeofday BEFORE our CF_TIME callback fires.
- ChronosBridge: phone via Chronos app → notification relay (app + message
  body; the library's splitTitle puts real content in `message` and the app
  name in `title` when the text has no early colon — title-first logic used
  to relay the placeholder "Message: Message" relic),
  Notice) + battery to the phone. Starts at boot.
- **Phone time skew ROOT-CAUSED + FIXED 2026-09-19**: the app's packet is
  CORRECT (raw dump decoded: `AB 00 0B FF 93 80 00 07 EA 09 13 <hh> <mm>
  <ss>` — the +1h came from ChronosESP32 applying components via
  ESP32Time::setTime which built `struct tm` with **tm_isdst = 0**
  ("standard time"), so mktime converted at CST (+6) while readback applied
  CDT (−5) → +1h whenever DST is active. **Local patch in
  .pio/libdeps/.../ESP32Time/ESP32Time.cpp line ~71: tm_isdst = -1** —
  VALIDATED on hardware (packet hour now parses correctly). The temporary
  raw-dump patch in ChronosESP32.cpp has been REMOVED. Policy: NTP is
  authoritative (phone push rolled back while NTP valid; accepted only
  pre-NTP). NOTE: the tm_isdst patch is lost if the library reinstalls —
  re-apply or vendor the libs. Coex connect robustness: client connect
  timeout raised 5s → 10s (status 13 = BLE_HS_ETIMEOUT under 3-link load).
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
- **Firmware identified 2026-09-19 (gatt dump)**: clock runs factory
  **app 0.8.56** (fw rev "0.8.56", sw rev `0503029A 0009 008C 0008 0038` =
  bl 0.9, SD 8C, app 0.8, build 56), hw rev 5.3 (0x2A27), model "666".
  This explains the 0x81 rejections: HA/C# users typically run 1.5.x+.
  Matching DFU zip for hw 5.3:
  `glance_firmware_full_0503029A0011008C0105000A.zip` (bl 0.17, app 1.5,
  build 10) in Hypfer/glance-clock-assets/firmwares (the 0601 zips are for
  newer hardware). Flash via nRF Connect/nRF Toolbox DFU. Bond may not
  survive the update — re-run `pair` (PIN flow) if reconnect fails; hands
  may need recalibration (`calib`/`calok`).
- **Scene char (5075ffac) has a hard write-length limit**: writing the
  80-byte forecast frame → rc 269 = 0x100+0x0D =
  BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN. Dead end for large scene frames
  regardless of firmware; the `wxs` probe was removed after the DFU made
  the data path work. `raws <hex>` remains for small manual probes and
  sendSceneCommand no longer masks rejections as no-response success.
  State char (5075fc78) read 0x04 this session (was 0x00 earlier —
  meaning unknown, possibly scene/mode indicator).
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
  (scene carousel nav — C#-VALIDATED: 30 = "previous face", 31 = "next
  face", the C# weather carousel advances with 31 every 15s; neither
  dismisses a scene, the watchface is just slot 0 in the carousel).
  4-byte frames for 35 are rejected (ATT 0x81); single-byte only.
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
3. Phase 3 backlog: clock battery → Chronos app (VALIDATED: 98% pushed on
   connect + on change). Then ForecastScene (built, see section 1), alarms
   (Chronos Alarm struct → Alarms protobuf), CallScene from ringer callback.
   All protobufs already generated in lib/GlanceCore. The C#
   GlanceProtocol.cs has validated ring builders to port (NumbersRingCommand
   for rain/wind/humidity rings). BUILT (untested): web brightness slider +
   night-mode toggle (both = settings RMW via GlanceClient::completeSettings,
   hoisted from Console for the portal), firmware rev on the web status page
   (read at connect from 0x180A/0x2A26, also in console `status`).

## 4a. Future investigation: clock battery drain while connected

User observation (2026-09-20): the clock's battery drains faster than
expected while bridged. What we actually do to the clock: one persistent
BLE connection, a battery-notify subscription (no polling), rare writes
(forecast 2x/hour max, settings RMW + read-back, CTS nudge on time
change). The dominant suspect is the CONNECTION ITSELF: the clock's
nRF52 wakes for every connection event, and the default interval is
likely short (peripheral-dictated or ~30ms-ish from pairing).

Levers to try (in order):
1. **Longer connection interval** — the central can request it after
   connect: `NimBLEClient::updateConnParams(min, max, latency, timeout)`
   (verified present in NimBLE 2.5.1). Try min/max ~300-500 ms with
   latency 2-4 + a safe supervision timeout. Watch: notices still arrive
   (they go over the connection), battery notify still arrives.
2. **Measure first**: 24 h battery % with the bridge connected vs clock
   idle-disconnected, to quantify the connection's share.
3. **Intermittent connection** (bigger change): connect only to push
   (forecast/notice/alarm) and disconnect after N s idle — the clock
   re-polls CTS on every connect anyway, so time stays correct; battery
   notify would be lost while disconnected.
4. Check whether the clock firmware sleeps less with multiple bonds/
   services active (8e400001 subscription adds CCCD work; `sub off` when
   unused).

Note: heap telemetry from 2026-09-20 sits in main loop + Forecast fetch
for the crash hunt; unrelated to this but same session.

## 4b. Future investigation: WiFi setup AP (headless provisioning)

User request (2026-09-20): when the ESP32 can't connect to WiFi (no
credentials, or repeated auth failures), spin up its own WiFi network so
a phone can join it and configure the real credentials — no laptop.

Sketch:
1. Trigger: WiFi not connected ~3 min after boot, either because
   credentials are empty or after N failed attempts (NetTime tracks the
   status transitions already).
2. SoftAP: `WiFi.mode(WIFI_AP_STA)` + SSID `GlanceBridge-Setup-XXXX`
   (XXXX = last 2 MAC bytes, so multiple bridges are distinguishable),
   open network, keep STA attempts going in the background.
3. Web portal already exists — extend `WebPortal::handle()` with an AP
   branch (it currently no-ops unless WL_CONNECTED): serve on the SoftAP
   interface (192.168.4.1), add a `/wifi` page listing
   `WiFi.scanNetworks()` results with a password form posting to
   NetTime::setCredentials, then drop the AP and reconnect.
4. DNSServer (captive portal: point every DNS query at us) so phones pop
   the portal automatically; mDNS is unreliable in AP mode — advertise
   the raw 192.168.4.1 on the clock display via a Notice if useful.
5. Caveats: AP+STA+BLE coexistence on the classic ESP32 degrades the
   clock link during setup (acceptable); an open setup portal is only a
   risk while provisioning (it appears only when WiFi is down) — still,
   consider only enabling it when credentials are absent or failing, not
   during normal operation.

## 5. Handy facts

- NimBLE callbacks run on the host task: NEVER call blocking BLE ops
  (secureConnection, readValue, writeValue) inside them. Pairing flow does
  everything in main task after connect() returns (see GlanceClient).
  VIOLATED TWICE (2026-09-19, both fixed): CF_ALARM → pushAlarms (blocking
  write) → "Stack canary watchpoint triggered (nimble_host)" crash when
  setting an alarm in the app; phone-time policy hook → esp_sntp_restart
  (lwIP) → "assert failed: udp_new_ip_type ... Required to lock TCPIP core"
  at boot. ChronosBridge callbacks now only set pending flags
  (_phoneTimePending/_alarmPushPending/_noticePending + fixed char buffer);
  loop() drains them on the main task. The notification relay was also
  moved off the host task (it had been lucky, not correct).
- `writeValue` with response blocks until ACK or the host's hard-coded 30s
  GATT timeout (`BLE_GATTC_UNRESPONSIVE_TIMEOUT_MS`), which then drops the
  connection. Every sendCommand logs `sending cmd 0xNN` first for visibility.
- NimBLE 2.x discovery is lazy: connect() does NOT populate the service
  cache; `discoverAttributes()` must be called for a full dump; getService()
  does its own filtered discovery.
- `NimBLEDevice::init()` is idempotent (safe when ChronosESP32::begin()
  calls it again); both roles share the NimBLE server singleton.
- **Bond vs DFU**: the firmware DFU wipes the CLOCK's bond store; the
  ESP32 keeps its stored address, so "reconnects" demand a PIN. On a PIN
  request during a non-pairing reconnect, GlanceClient explains this and
  lets the user re-pair through the normal PIN flow (re-establishes both
  sides). Boot log prints ESP32-side bond presence
  (`NimBLEDevice::getNumBonds()` scan for the stored address) so a
  MISSING bond (NVS wiped on our side) is distinguishable from a clock-side
  bond loss. If the PIN is demanded after EVERY ESP32 reflash while the
  boot log says "bond: present", the CLOCK is dropping the bond on
  central disconnect (firmware behavior) — not our NVS.
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
