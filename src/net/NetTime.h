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

class NetTime {
public:
    void begin();  // applies stored credentials + timezone, starts WiFi
    void loop();   // monitors link + reports first successful NTP sync

    bool setCredentials(const String& ssid, const String& pass);
    void clearCredentials();
    void setTimezone(const String& tz);

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
};
