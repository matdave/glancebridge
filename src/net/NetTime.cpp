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

// Headless provisioning (NOTES.md 4b): open the setup AP when the STA
// link stays down - quickly when nothing is stored, slower when stored
// credentials keep failing (they get a fair retry window first).
static const uint32_t SETUP_TRIGGER_MS = 180000;          // failing credentials
static const uint32_t SETUP_TRIGGER_NO_CREDS_MS = 30000;  // nothing stored
static const char* SETUP_SSID_PREFIX = "GlanceBridge-Setup-";

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
    } else {
        Serial.printf("[%s] no WiFi credentials - setup portal opens in %lu s "
                      "(or: wifi <ssid> <pass>)\n", TAG,
                      (unsigned long)(SETUP_TRIGGER_NO_CREDS_MS / 1000));
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

    // Headless provisioning trigger: link down continuously -> open the
    // setup AP (30 s with nothing stored, 3 min when credentials exist but
    // keep failing). Brief flaps keep resetting the timer; the AP closes
    // the moment the link comes back.
    if (st == WL_CONNECTED) {
        if (_setupAp) {
            Serial.printf("[%s] connected to '%s' - closing setup portal\n",
                          TAG, _ssid.c_str());
            stopSetupAp();
        }
        _downSinceMs = 0;
    } else if (_downSinceMs == 0) {
        _downSinceMs = millis();
    } else if (!_setupAp) {
        uint32_t trigger =
            _ssid.length() ? SETUP_TRIGGER_MS : SETUP_TRIGGER_NO_CREDS_MS;
        if ((int32_t)(millis() - _downSinceMs) >= (int32_t)trigger) {
            Serial.printf("[%s] WiFi down for %lu s - opening setup portal\n",
                          TAG, (unsigned long)((millis() - _downSinceMs) / 1000));
            startSetupAp();
        }
    }

    // Link-loss watchdog. WiFi.setAutoReconnect() handles a plain drop, but
    // a wedged lwIP stack (see NOTES.md: the udp_new_ip_type panic) leaves
    // WiFi dead while the rest of the ESP runs. If we were connected and the
    // link has stayed down past the grace window, force the same clean
    // disconnect+begin() sequence setCredentials() uses.
    if (_ssid.length() && !_setupAp) {  // paused while the portal is up
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
    // The setup portal closes first (no-op if it was off): STA attempts
    // would hop channels and break the AP for the phone that just saved.
    stopSetupAp();
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

void NetTime::startSetupAp() {
    if (_setupAp) {
        return;
    }
    _setupAp = true;
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    _setupSsid = String(SETUP_SSID_PREFIX) + mac.substring(mac.length() - 4);
    // Pause STA while the portal is up: connect attempts (and auto-reconnect)
    // scan every channel and the softAP drops off its channel with them -
    // phones get kicked off the setup network mid-session. STA resumes when
    // the AP closes (stopSetupAp / setCredentials).
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_AP_STA);
    // Pre-scan BEFORE the AP exists: a scan hops channels and would kick
    // connected phones - but there are no clients yet. The list is then
    // ready on the first /wifi load; later rescans are user-invoked.
    Serial.println("[NetTime] pre-scan for the setup portal...");
    WiFi.scanDelete();
    WiFi.scanNetworks(false, false);  // blocking ~2-3 s, no clients yet
    WiFi.softAP(_setupSsid.c_str());  // open network: the portal itself is the gate
    Serial.printf("[%s] setup portal live: join open network '%s', then open "
                  "http://%s/ (any URL redirects there)\n",
                  TAG, _setupSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void NetTime::stopSetupAp() {
    if (!_setupAp) {
        return;
    }
    _setupAp = false;
    WiFi.softAPdisconnect(true);
    WiFi.setAutoReconnect(true);
    // Back to STA-only; an established STA link survives the mode change.
    WiFi.mode(WIFI_STA);
    if (_ssid.length() && WiFi.status() != WL_CONNECTED) {
        // STA was paused for the portal; bring the stored credentials back.
        WiFi.begin(_ssid.c_str(), _pass.c_str());
    }
    _downSinceMs = 0;  // fresh grace window before the AP may re-open
    Serial.printf("[%s] setup portal closed\n", TAG);
}

void NetTime::setTimezone(const String& tz) {
    _prefs.begin(NVS_NS, false);
    _prefs.putString("tz", tz);
    _prefs.end();
    _tz = tz;
    _timeValid = false;  // re-report after the next sync with the new tz
    applyTz();
}
