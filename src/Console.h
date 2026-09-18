#pragma once

#include <Arduino.h>
#include <glance/GlanceCommands.h>

#include "bridge/ChronosBridge.h"
#include "glance/GlanceClient.h"

// Minimal serial command interface for hardware bring-up and testing.
// Type "help" for the command list.

class Console {
public:
    Console(GlanceClient& client, ChronosBridge& bridge) : _client(client), _bridge(bridge) {}

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
            // CR, LF or CRLF all end a line (empty lines are ignored, so a
            // CRLF pair only executes the command once).
            if (c == '\r' || c == '\n') {
                if (_idx == 0) continue;
                _line[_idx] = '\0';
                Serial.println();
                handle(_line);
                _idx = 0;
                Serial.print("> ");
                return;
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
        Serial.println("  stop               scenes stop (previous slot)");
        Serial.println("  start              scenes start (next slot)");
        Serial.println("  clear              clear all scenes");
        Serial.println("  bonds              clear pairings stored in the clock");
        Serial.println("  refresh            UpdateAndRefresh");
        Serial.println("  night on|off       automatic night mode");
        Serial.println("  gatt               dump the clock's GATT table");
        Serial.println("  sub on|off         (un)subscribe to the undocumented 8e400001 channel");
        Serial.println("  settings           re-request the clock's settings");
        Serial.println("  time               show the ESP32's local time");
        Serial.println("  settime ...        set local time: settime YYYY-MM-DD HH:MM:SS");
        Serial.println("  chronos on|off     start the Chronos peripheral / pause the relay");
        Serial.println("  forget             forget stored clock address");
        Serial.println("  status             connection + settings");
        Serial.println("  raw <hex>          write raw bytes to the data characteristic");
        Serial.println("  help               this list");
        Serial.println();
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
            _client.sendCommand(Glance::Cmd::ScenesStop, Glance::ScenePriority::BandSystem);
        } else if (cmd == "start") {
            _client.sendCommand(Glance::Cmd::ScenesStart, Glance::ScenePriority::BandSystem);
        } else if (cmd == "clear") {
            _client.sendCommand(Glance::Cmd::ScenesClear, Glance::ScenePriority::BandSystem);
        } else if (cmd == "bonds") {
            _client.sendCommand(Glance::Cmd::BondsClear, Glance::ScenePriority::BandSystem);
        } else if (cmd == "refresh") {
            _client.sendCommand(Glance::Cmd::UpdateAndRefresh, Glance::ScenePriority::BandSystem);
        } else if (cmd == "night" && arg == "on") {
            _client.sendCommand(Glance::Cmd::EnableAutomaticNightMode, Glance::ScenePriority::BandSystem);
        } else if (cmd == "night" && arg == "off") {
            _client.sendCommand(Glance::Cmd::DisableAutomaticNightMode, Glance::ScenePriority::BandSystem);
        } else if (cmd == "gatt") {
            _client.dumpGattTable();
        } else if (cmd == "sub" && arg == "on") {
            _client.subscribePush(true);
        } else if (cmd == "sub" && arg == "off") {
            _client.subscribePush(false);
        } else if (cmd == "settings") {
            _client.readSettings();
        } else if (cmd == "time") {
            struct tm t;
            if (getLocalTime(&t)) {
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
                Serial.printf("[console] local time: %s\n", buf);
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
            } else {
                Serial.println("[console] usage: settime YYYY-MM-DD HH:MM:SS");
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
        } else if (cmd == "raw") {
            handleRaw(arg.c_str());
        } else {
            Serial.printf("[console] unknown command '%s' (try 'help')\n", cmd.c_str());
        }
    }

    GlanceClient& _client;
    ChronosBridge& _bridge;
    char _line[128] = {0};
    size_t _idx = 0;
};
