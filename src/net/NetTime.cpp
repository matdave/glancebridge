#include <net/NetTime.h>

#include <esp_sntp.h>

static const char* TAG = "NetTime";
static const char* NVS_NS = "nettime";

static const char* NTP_SERVER_1 = "time.nist.gov";
static const char* NTP_SERVER_2 = "pool.ntp.org";
static const char* NTP_SERVER_3 = "time.google.com";

// How long to let WiFi's built-in auto-reconnect try before we force a
// clean reconnect, and the minimum gap between forced reconnects.
static const uint32_t LINK_LOST_GRACE_MS = 30000;
static const uint32_t FORCE_RECONNECT_COOLDOWN_MS = 60000;

void NetTime::begin() {
    _prefs.begin(NVS_NS, true);
    _ssid = _prefs.getString("ssid", "");
    _pass = _prefs.getString("pass", "");
    _tz = _prefs.getString("tz", "");
    _prefs.end();

    WiFi.setAutoReconnect(true);
    // Modem sleep off: WiFi + dual-role BLE coexistence is more reliable
    // without it (slightly higher power draw, fine for this project).
    WiFi.setSleep(false);
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

    wl_status_t st = WiFi.status();
    if (st != _lastStatus) {
        _lastStatus = st;
        // Numeric code included: WL_IDLE_STATUS=0, WL_NO_SSID_AVAIL=1,
        // WL_SCAN_COMPLETED=2, WL_CONNECTED=3, WL_CONNECT_FAILED=4,
        // WL_CONNECTION_LOST=5, WL_DISCONNECTED=6.
        Serial.printf("[%s] WiFi status -> %d (%s), IP: %s\n", TAG, (int)st,
                      statusName(st), WiFi.localIP().toString().c_str());
    }

    // Link-loss watchdog. WiFi.setAutoReconnect() handles a plain drop, but
    // a wedged lwIP stack (see NOTES.md: the udp_new_ip_type panic) leaves
    // WiFi dead while the rest of the ESP runs. If we were connected and the
    // link has stayed down past the grace window, force the same clean
    // disconnect+begin() sequence setCredentials() uses.
    if (_ssid.length()) {
        if (st == WL_CONNECTED) {
            _wasConnected = true;
            _linkLostAtMs = 0;
        } else if (_wasConnected) {
            uint32_t now = millis();
            if (_linkLostAtMs == 0) {
                _linkLostAtMs = now;
                Serial.printf("[%s] link lost (status %d %s, rssi %d dBm) - "
                              "auto-reconnect grace %lu s\n",
                              TAG, (int)st, statusName(st), WiFi.RSSI(),
                              (unsigned long)(LINK_LOST_GRACE_MS / 1000));
            } else if ((int32_t)(now - _linkLostAtMs) >= (int32_t)LINK_LOST_GRACE_MS &&
                       (int32_t)(now - _lastForcedReconnectMs) >=
                           (int32_t)FORCE_RECONNECT_COOLDOWN_MS) {
                Serial.printf("[%s] link still down %lu s (status %d %s, "
                              "heap %u) - forcing clean reconnect to '%s'\n",
                              TAG, (unsigned long)((now - _linkLostAtMs) / 1000),
                              (int)st, statusName(st), (unsigned)ESP.getFreeHeap(),
                              _ssid.c_str());
                WiFi.disconnect(false, false);
                WiFi.setSleep(false);
                WiFi.mode(WIFI_STA);
                delay(100);
                WiFi.begin(_ssid.c_str(), _pass.c_str());
                _lastForcedReconnectMs = now;
                _linkLostAtMs = now;  // fresh grace window for this attempt
            }
        }
    }

    if (_sntpStarted && !_timeValid) {
        struct tm t;
        if (getLocalTime(&t, 0)) {
            _timeValid = true;
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
            Serial.printf("[%s] time synced via NTP: %s\n", TAG, buf);
            _lastGoodEpoch = time(nullptr);
        }
    } else if (_timeValid) {
        // Keep a fresh snapshot so a bad external time source (phone app)
        // can be rolled back instantly.
        _lastGoodEpoch = time(nullptr);
    }
}

bool NetTime::restoreTime() {
    if (_lastGoodEpoch < 100000000LL) {  // pre-1973: nothing sane to restore
        return false;
    }
    struct timeval tv = {_lastGoodEpoch, 0};
    settimeofday(&tv, nullptr);
    esp_sntp_restart();  // converge to the exact NTP time
    return true;
}

const char* NetTime::statusName(wl_status_t st) const {
    switch (st) {
        case WL_IDLE_STATUS: return "idle";
        case WL_NO_SSID_AVAIL: return "no ssid";
        case WL_SCAN_COMPLETED: return "scan done";
        case WL_CONNECTED: return "connected";
        case WL_CONNECT_FAILED: return "connect failed";
        case WL_CONNECTION_LOST: return "lost";
        default: return "disconnected";
    }
}

bool NetTime::setCredentials(const String& ssid, const String& pass) {
    _prefs.begin(NVS_NS, false);
    _prefs.putString("ssid", ssid);
    _prefs.putString("pass", pass);
    _prefs.end();
    _ssid = ssid;
    _pass = pass;
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
        Serial.printf("[%s] already connected to '%s'\n", TAG, ssid.c_str());
        if (!_sntpStarted) {
            applyTz();
        }
        return true;
    }
    // Abort any in-flight attempt cleanly before restarting: begin() right
    // after a disconnect()/failed attempt returned WL_CONNECT_FAILED (4)
    // once, while the same credentials connect fine at boot.
    WiFi.disconnect(false, false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    delay(100);
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
