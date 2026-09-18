#include <net/NetTime.h>

static const char* TAG = "NetTime";
static const char* NVS_NS = "nettime";

static const char* NTP_SERVER_1 = "time.nist.gov";
static const char* NTP_SERVER_2 = "pool.ntp.org";
static const char* NTP_SERVER_3 = "time.google.com";

void NetTime::begin() {
    _prefs.begin(NVS_NS, true);
    _ssid = _prefs.getString("ssid", "");
    _pass = _prefs.getString("pass", "");
    _tz = _prefs.getString("tz", "");
    _prefs.end();

    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);

    if (_ssid.length()) {
        WiFi.begin(_ssid.c_str(), _pass.c_str());
        Serial.printf("[%s] connecting to WiFi '%s'...\n", TAG, _ssid.c_str());
        applyTz();
    }
}

void NetTime::applyTz() {
    String tz = _tz.length() ? _tz : "UTC0";
    configTzTime(tz.c_str(), NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    _sntpStarted = true;
    Serial.printf("[%s] SNTP started (tz: %s, servers: %s %s %s)\n", TAG, tz.c_str(),
                  NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

void NetTime::loop() {
    if ((int32_t)(millis() - _nextCheckMs) < 0) {
        return;
    }
    _nextCheckMs = millis() + 5000;

    bool connected = WiFi.isConnected();
    if (connected != _wasConnected) {
        _wasConnected = connected;
        if (connected) {
            Serial.printf("[%s] WiFi connected, IP: %s\n", TAG,
                          WiFi.localIP().toString().c_str());
        } else {
            Serial.printf("[%s] WiFi disconnected\n", TAG);
        }
    }

    if (_sntpStarted && !_timeValid) {
        struct tm t;
        if (getLocalTime(&t, 0)) {
            _timeValid = true;
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
            Serial.printf("[%s] time synced via NTP: %s\n", TAG, buf);
        }
    }
}

bool NetTime::setCredentials(const String& ssid, const String& pass) {
    _prefs.begin(NVS_NS, false);
    _prefs.putString("ssid", ssid);
    _prefs.putString("pass", pass);
    _prefs.end();
    _ssid = ssid;
    _pass = pass;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    if (!_sntpStarted) {
        applyTz();
    }
    Serial.printf("[%s] WiFi credentials saved, connecting to '%s'\n", TAG, ssid.c_str());
    return true;
}

void NetTime::clearCredentials() {
    _prefs.begin(NVS_NS, false);
    _prefs.remove("ssid");
    _prefs.remove("pass");
    _prefs.end();
    _ssid = "";
    _pass = "";
    WiFi.disconnect(false, true);
    Serial.printf("[%s] WiFi credentials cleared\n", TAG);
}

void NetTime::setTimezone(const String& tz) {
    _prefs.begin(NVS_NS, false);
    _prefs.putString("tz", tz);
    _prefs.end();
    _tz = tz;
    _timeValid = false;  // re-report after the next sync with the new tz
    applyTz();
}
