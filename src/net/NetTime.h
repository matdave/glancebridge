#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

// WiFi + SNTP time source.
//
// The Glance clock expects local wall-clock time and has no timezone
// handling, so a POSIX TZ string is applied before SNTP starts
// (configTzTime). Once NTP syncs, the system clock - and therefore the
// Current Time Service served to the clock - is correct without the
// Chronos phone app.
//
// Credentials and timezone are stored in NVS (plaintext; acceptable for
// this device class). Configure over serial:
//   wifi <ssid> <password>
//   tz <posix tz string>    e.g. tz EST5EDT,M3.2.0,M11.1.0
//
// Headless provisioning: when the STA link stays down (no credentials, or
// credentials that keep failing) the bridge opens an open setup AP
// ("GlanceBridge-Setup-XXXX") and the web portal serves a /wifi page on it
// with captive DNS - see startSetupAp() and NOTES.md 4b.

class NetTime {
public:
    void begin();  // applies stored credentials + timezone, starts WiFi
    void loop();   // monitors link + reports first successful NTP sync

    bool setCredentials(const String& ssid, const String& pass);
    void clearCredentials();
    void setTimezone(const String& tz);

    // Setup AP for headless WiFi provisioning. Started automatically when
    // the link stays down (30 s with nothing stored, 3 min with failing
    // credentials); also drivable manually ('wifiap on|off' over serial).
    // While active the STA side is PAUSED - its connect attempts hop
    // channels and would break the AP for connected phones.
    bool setupActive() const { return _setupAp; }
    String setupSsid() const { return _setupSsid; }
    void startSetupAp();
    void stopSetupAp();

    bool isConnected() const { return WiFi.isConnected(); }
    bool timeValid() const { return _timeValid; }
    String ssid() const { return _ssid; }
    String tz() const { return _tz; }

    // Roll the system time back to the last NTP-derived value (used to
    // reject a bad phone time push while NTP is authoritative). False when
    // no sane snapshot exists.
    bool restoreTime();

private:
    void applyTz();
    const char* statusName(wl_status_t st) const;

    Preferences _prefs;
    String _ssid;
    String _pass;
    String _tz;
    bool _sntpStarted = false;
    bool _timeValid = false;
    wl_status_t _lastStatus = WL_DISCONNECTED;
    int64_t _lastGoodEpoch = 0;
    uint32_t _nextCheckMs = 0;

    // Link-loss watchdog: WiFi.setAutoReconnect() recovers a plain drop,
    // but a wedged lwIP stack (see the old udp_new_ip_type panic) leaves
    // WiFi dead with the rest of the ESP alive. If the link is still down
    // after a grace window, force the clean disconnect+begin() sequence.
    bool _wasConnected = false;
    uint32_t _linkLostAtMs = 0;
    uint32_t _lastForcedReconnectMs = 0;

    // Headless provisioning state (see setupActive()).
    bool _setupAp = false;
    String _setupSsid;
    uint32_t _downSinceMs = 0;  // start of the current continuous down period
};
