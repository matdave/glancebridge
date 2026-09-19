#pragma once

#include <Arduino.h>
#include <glance/GlanceCommands.h>
#include <glance/GlanceMessages.h>
#include <WiFi.h>

#include "bridge/ChronosBridge.h"
#include "glance/GlanceClient.h"
#include "net/NetTime.h"

// Minimal serial command interface for hardware bring-up and testing.
// Type "help" for the command list.

class Console {
public:
    Console(GlanceClient& client, ChronosBridge& bridge, NetTime& net)
        : _client(client), _bridge(bridge), _net(net) {}

    void begin(unsigned long baud = 115200) {
        Serial.begin(baud);
        _client.setPinProvider([]() -> uint32_t {
            // Drain input typed ahead while blocking ops were running,
            // otherwise a stale Enter would submit an empty PIN instantly.
            while (Serial.available() > 0) {
                Serial.read();
            }
            Serial.println();
            Serial.println("=============================================");
            Serial.println("Enter the PIN shown on the clock, then Enter:");
            Serial.println("(you have 25 seconds - BLE pairing timeout)");
            Serial.println("=============================================");

            uint32_t start = millis();
            while (millis() - start < 25000) {
                char line[16];
                if (!readLine(line, sizeof(line), 25000 - (millis() - start))) {
                    break;
                }
                uint32_t passkey = 0;
                int digits = 0;
                for (const char* p = line; *p; p++) {
                    if (*p >= '0' && *p <= '9') {
                        passkey = passkey * 10 + (*p - '0');
                        digits++;
                    } else if (*p != ' ') {
                        digits = 0;
                        break;
                    }
                }
                if (digits >= 1 && digits <= 6) {
                    Serial.printf("[console] PIN entered: %06u\n", (unsigned)passkey);
                    return passkey;
                }
                Serial.println("[console] empty or invalid PIN - type the digits shown on the clock");
            }
            Serial.println("[console] PIN entry timed out");
            return 0;
        });
        printHelp();
        Serial.print("> ");
    }

    void poll() {
        while (Serial.available() > 0) {
            char c = (char)Serial.read();
            // Terminal line-editing keys: swallow ANSI escapes (arrow keys,
            // history recall send ESC [ <x>) and treat BS/DEL as erase.
            if (_ansi == 2) {  // final byte of an ESC [ x sequence
                _ansi = 0;
                continue;
            }
            if (_ansi == 1) {
                if (c == '[' || c == 'O') {
                    _ansi = 2;
                } else {
                    _ansi = 0;
                }
                continue;
            }
            if (c == 0x1b) {
                _ansi = 1;
                continue;
            }
            if (c == '\r' || c == '\n') {
                // CR, LF or CRLF all end a line (empty lines are ignored, so
                // a CRLF pair only executes the command once).
                if (_idx == 0) continue;
                _line[_idx] = '\0';
                Serial.println();
                handle(_line);
                _idx = 0;
                Serial.print("> ");
                return;
            }
            if (c == 0x08 || c == 0x7f) {
                if (_idx > 0) {
                    _idx--;
                    Serial.print("\b \b");
                }
                continue;
            }
            if (_idx + 1 < sizeof(_line)) {
                _line[_idx++] = c;
                Serial.print(c);  // echo typed characters
            }
        }
    }

private:
    static bool readLine(char* buf, size_t bufLen, uint32_t timeoutMs) {
        size_t idx = 0;
        uint32_t start = millis();
        while (millis() - start < timeoutMs) {
            while (Serial.available() > 0) {
                char c = (char)Serial.read();
                if (c == '\r' || c == '\n') {  // CR, LF or CRLF all end the line
                    buf[idx] = '\0';
                    Serial.println();
                    return true;
                }
                if (idx + 1 < bufLen) {
                    buf[idx++] = c;
                    Serial.print(c);
                }
            }
            delay(10);
        }
        buf[idx] = '\0';
        return false;
    }

    static void printHelp() {
        Serial.println();
        Serial.println("Glance Clock bridge commands:");
        Serial.println("  scan [ms]          scan for Glance clocks (default 5000)");
        Serial.println("  pair               connect + pair (press clock's pairing button first)");
        Serial.println("  notify <text>      show a notification on the clock");
        Serial.println("  stop|start         scene slot navigation (single-byte cmds 30/31)");
        Serial.println("  clear              clear all scenes");
        Serial.println("  bonds              clear pairings stored in the clock");
        Serial.println("  night on|off       night mode (settings write)");
        Serial.println("  calib              start hands calibration (clock spins hands to 12)");
        Serial.println("  calok              confirm hands calibration");
        Serial.println("  gatt               dump the clock's GATT table");
        Serial.println("  sub on|off         (un)subscribe to the undocumented 8e400001 channel");
        Serial.println("  settings           read the clock's published settings");
        Serial.println("  cfg [0-255]        write settings (read-modify-write; optional brightness)");
        Serial.println("  batt               read the clock's battery level");
        Serial.println("  time               show the ESP32's local time");
        Serial.println("  settime ...        set local time: settime YYYY-MM-DD HH:MM:SS");
        Serial.println("  chronos on|off     start the Chronos peripheral / pause the relay");
        Serial.println("  wifi <ssid> <pass> connect to WiFi and sync time via NTP");
        Serial.println("  wifioff            clear stored WiFi credentials");
        Serial.println("  tz [posix]         show/set timezone, e.g. tz EST5EDT,M3.2.0,M11.1.0");
        Serial.println("  forget             forget stored clock address");
        Serial.println("  status             connection + settings");
        Serial.println("  raw <hex>          write raw bytes to the data characteristic");
        Serial.println("  help               this list");
        Serial.println();
    }

    // Full settings message for a settings write: starts from the clock's
    // last known values (falls back to safe defaults for fields the clock
    // has not published), preserves DND/silent schedules when known, and
    // sets every has_* flag so the write replaces nothing accidentally.
    static Settings completeSettings(const Settings& src) {
        Settings s = Settings_init_default;
        s.has_nightModeEnabled = true;
        s.nightModeEnabled = src.has_nightModeEnabled ? src.nightModeEnabled : true;
        s.has_permanentDND = true;
        s.permanentDND = src.has_permanentDND && src.permanentDND;
        s.has_permanentMute = true;
        s.permanentMute = src.has_permanentMute && src.permanentMute;
        s.has_dateFormat = true;
        s.dateFormat = src.has_dateFormat ? src.dateFormat
                                          : Settings_DateFormat_DateDisabled;
        s.has_pointsAlwaysEnabled = true;
        s.pointsAlwaysEnabled = src.has_pointsAlwaysEnabled && src.pointsAlwaysEnabled;
        s.has_displayBrightness = true;
        s.displayBrightness = src.has_displayBrightness ? src.displayBrightness : 128;
        s.has_timeModeEnable = true;
        s.timeModeEnable = src.has_timeModeEnable ? src.timeModeEnable : true;
        s.has_timeFormat12 = true;
        s.timeFormat12 = src.has_timeFormat12 && src.timeFormat12;
        s.has_mgrUserActivityTimeout = true;
        s.mgrUserActivityTimeout =
            src.has_mgrUserActivityTimeout ? src.mgrUserActivityTimeout : 600;
        if (src.has_dnd) {
            s.has_dnd = true;
            s.dnd = src.dnd;
        }
        if (src.has_silent) {
            s.has_silent = true;
            s.silent = src.silent;
        }
        return s;
    }

    void handlePair() {
        // 10s scan: the clock advertises slowly after the pairing button
        if (_client.scan(10000) == 0 && !_client.hasStoredAddress()) {
            Serial.println("[console] no clocks found; is the clock advertising?");
            return;
        }
        _client.pair();
    }

    void handleRaw(const char* hex) {
        uint8_t buf[256];
        size_t len = 0;
        int hi = -1;
        for (const char* p = hex; *p && len < sizeof(buf); p++) {
            int v;
            if (*p >= '0' && *p <= '9') v = *p - '0';
            else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
            else continue;
            if (hi < 0) {
                hi = v;
            } else {
                buf[len++] = (uint8_t)((hi << 4) | v);
                hi = -1;
            }
        }
        Serial.printf("[console] writing %u bytes\n", (unsigned)len);
        _client.sendCommand(buf, len);
    }

    void handle(const char* input) {
        while (*input == ' ') input++;
        String cmd(input);
        int space = cmd.indexOf(' ');
        String arg = space >= 0 ? cmd.substring(space + 1) : String();
        cmd = space >= 0 ? cmd.substring(0, space) : cmd;

        if (cmd.length() == 0) return;

        if (cmd == "help") {
            printHelp();
        } else if (cmd == "scan") {
            uint32_t ms = arg.length() ? (uint32_t)arg.toInt() : 5000;
            _client.scan(ms);
        } else if (cmd == "pair") {
            handlePair();
        } else if (cmd == "notify") {
            if (arg.length() == 0) {
                Serial.println("[console] usage: notify <text>");
            } else if (_client.sendNotice(arg.c_str())) {
                Serial.println("[console] notify sent");
            }
        } else if (cmd == "stop") {
            const uint8_t c = Glance::Cmd::ScenesStop;  // single byte, like the C# client
            _client.sendCommand(&c, 1);
        } else if (cmd == "start") {
            const uint8_t c = Glance::Cmd::ScenesStart;  // single byte
            _client.sendCommand(&c, 1);
        } else if (cmd == "clear") {
            _client.sendCommand(Glance::Cmd::ScenesClear, Glance::ScenePriority::BandSystem);
        } else if (cmd == "bonds") {
            _client.sendCommand(Glance::Cmd::BondsClear, Glance::ScenePriority::BandSystem);
        } else if (cmd == "calib") {
            const uint8_t c = Glance::Cmd::StartCalibration;  // single byte
            _client.sendCommand(&c, 1);
        } else if (cmd == "calok") {
            const uint8_t c = Glance::Cmd::ConfirmCalibration;  // single byte
            _client.sendCommand(&c, 1);
        } else if (cmd == "night" && (arg == "on" || arg == "off")) {
            // Night mode via settings write (the 40/41 command frames are
            // from the cloud era and rejected by this firmware).
            Settings s = completeSettings(_client.lastSettings());
            s.nightModeEnabled = (arg == "on");
            Serial.printf("[console] writing settings with nightMode=%d\n",
                          s.nightModeEnabled ? 1 : 0);
            if (_client.writeSettings(&s)) {
                Serial.println("[console] settings written - now run 'settings'");
            }
        } else if (cmd == "night") {
            Serial.println("[console] usage: night on|off");
        } else if (cmd == "gatt") {
            _client.dumpGattTable();
        } else if (cmd == "sub" && arg == "on") {
            _client.subscribePush(true);
        } else if (cmd == "sub" && arg == "off") {
            _client.subscribePush(false);
        } else if (cmd == "settings") {
            _client.readSettings();
        } else if (cmd == "cfg") {
            // Read-modify-write settings write (replaces everything on the
            // clock, based on the last decoded values). Optional brightness
            // makes the change visible; 'settings' afterwards re-reads.
            Settings s = completeSettings(_client.lastSettings());
            bool valid = true;
            for (const char* p = arg.c_str(); *p; p++) {
                if (*p < '0' || *p > '9') {
                    valid = false;
                    break;
                }
            }
            long b = arg.toInt();
            if (arg.length() == 0) {
                // keep current/default brightness
            } else if (valid && b >= 0 && b <= 255) {
                s.displayBrightness = (int)b;
            } else {
                Serial.println("[console] usage: cfg [0-255] (brightness; omit to keep current)");
                return;
            }
            Serial.printf("[console] writing settings (night=%d dnd=%d mute=%d 24h=%d bright=%d)\n",
                          s.nightModeEnabled ? 1 : 0, s.permanentDND ? 1 : 0,
                          s.permanentMute ? 1 : 0, s.timeFormat12 ? 1 : 0,
                          s.displayBrightness);
            if (_client.writeSettings(&s)) {
                Serial.println("[console] settings written - now run 'settings'");
            }
        } else if (cmd == "batt") {
            _client.readBattery();
        } else if (cmd == "time") {
            struct tm t;
            if (getLocalTime(&t)) {
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
                Serial.printf("[console] local time: %s (ntp=%d wifi=%d)\n", buf,
                              _net.timeValid() ? 1 : 0, _net.isConnected() ? 1 : 0);
            } else {
                Serial.println("[console] system time not set");
            }
        } else if (cmd == "settime") {
            // usage: settime YYYY-MM-DD HH:MM:SS (local wall clock)
            int y, mo, d, h, mi, s;
            if (sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
                struct tm t = {};
                t.tm_year = y - 1900;
                t.tm_mon = mo - 1;
                t.tm_mday = d;
                t.tm_hour = h;
                t.tm_min = mi;
                t.tm_sec = s;
                time_t epoch = mktime(&t);
                struct timeval tv = {epoch, 0};
                settimeofday(&tv, nullptr);
                Serial.printf("[console] system time set to %s\n", arg.c_str());
                _client.refreshClockTime();
            } else {
                Serial.println("[console] usage: settime YYYY-MM-DD HH:MM:SS");
            }
        } else if (cmd == "wifi") {
            int sp = arg.indexOf(' ');
            if (sp <= 0 || sp + 1 >= (int)arg.length()) {
                Serial.println("[console] usage: wifi <ssid> <password>");
            } else {
                _net.setCredentials(arg.substring(0, sp), arg.substring(sp + 1));
            }
        } else if (cmd == "wifioff") {
            _net.clearCredentials();
        } else if (cmd == "tz") {
            if (arg.length()) {
                _net.setTimezone(arg);
            } else {
                Serial.printf("[console] timezone: %s\n",
                              _net.tz().length() ? _net.tz().c_str() : "UTC0 (default)");
                Serial.println("[console] set with e.g.: tz EST5EDT,M3.2.0,M11.1.0");
            }
        } else if (cmd == "chronos" && arg == "on") {
            _bridge.begin();
            _bridge.setRelay(true);
        } else if (cmd == "chronos" && arg == "off") {
            // pauses the relay only; the peripheral keeps running
            _bridge.setRelay(false);
        } else if (cmd == "forget") {
            _client.forget();
        } else if (cmd == "status") {
            Serial.printf(
                "[console] connected=%d stored=%s relay=%d phone=%d settings=%s\n",
                _client.isConnected() ? 1 : 0, _client.storedAddress().c_str(),
                _bridge.relayEnabled() ? 1 : 0, _bridge.phoneConnected() ? 1 : 0,
                _client.lastSettingsHex().c_str());
            Serial.printf("[console] wifi=%d(%s) ntp=%d heap=%u\n", (int)WiFi.status(),
                          _net.isConnected() ? "up" : "down", _net.timeValid() ? 1 : 0,
                          (unsigned)ESP.getFreeHeap());
            if (_client.lastBattery() >= 0) {
                Serial.printf("[console] clock battery (last): %d%%\n",
                              _client.lastBattery());
            }
        } else if (cmd == "raw") {
            handleRaw(arg.c_str());
        } else {
            Serial.printf("[console] unknown command '%s' (try 'help')\n", cmd.c_str());
            // Raw bytes of the line: a clean 'status' reported as unknown
            // means invisible chars (control bytes) reached the buffer.
            Serial.printf("[console] raw line bytes:");
            for (const char* p = input; *p; p++) {
                Serial.printf(" %02x", (uint8_t)*p);
            }
            Serial.println();
        }
    }

    GlanceClient& _client;
    ChronosBridge& _bridge;
    NetTime& _net;
    char _line[128] = {0};
    size_t _idx = 0;
    uint8_t _ansi = 0;  // ANSI escape-sequence parser state (0=none)
};
