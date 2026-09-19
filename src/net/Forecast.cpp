#include <net/Forecast.h>

#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

static const char* TAG = "Weather";

void Forecast::begin() {
    // Writable open creates the namespace so later read-only opens don't
    // log "nvs_open failed: NOT_FOUND" on fresh installs (before 'geo').
    _prefs.begin("weather", false);
    _lat = _prefs.getString("lat", "");
    _lon = _prefs.getString("lon", "");
    _unit = _prefs.getString("unit", "f");
    _prefs.end();
    _hasGeo = _lat.length() > 0 && _lon.length() > 0;
    if (_hasGeo) {
        Serial.printf("[%s] stored location: %s, %s (unit %s)\n", TAG, _lat.c_str(),
                      _lon.c_str(), _unit == "f" ? "F" : "C");
        _nextFetchMs = millis();  // fetch as soon as WiFi is up
    }
}

void Forecast::loop() {
    // Auto-hide: after the display window, DELETE the forecast scene (cmd
    // 33 ScenesDelete, [33,0,0,slot] per the C# client). With no carousel
    // scenes left the clock falls back to its native bright watchface -
    // navigating with 30/31 only cycles faces (and empty slots render dim,
    // as does the mode-8 watchface scene on this firmware).
    if (_hideAtMs != 0 && (int32_t)(millis() - _hideAtMs) >= 0) {
        _hideAtMs = 0;
        if (_glance.isConnected()) {
            const uint8_t del[] = {Glance::Cmd::ScenesDelete, 0, 0, FORECAST_SLOT};
            Serial.println("[Weather] forecast window over - deleting the scene");
            _glance.sendCommand(del, sizeof(del));
        }
    }

    if (!_hasGeo || _fetching || WiFi.status() != WL_CONNECTED) {
        return;
    }
    if ((int32_t)(millis() - _nextFetchMs) < 0) {
        return;
    }
    _nextFetchMs = millis() + 30UL * 60UL * 1000UL;
    fetch(nullptr);  // stored unit
}

bool Forecast::setLocation(const String& lat, const String& lon) {
    float la = lat.toFloat();
    float lo = lon.toFloat();
    if (la < -90.0f || la > 90.0f || lo < -180.0f || lo > 180.0f ||
        (la == 0.0f && lat != "0")) {
        Serial.printf("[%s] invalid location '%s %s'\n", TAG, lat.c_str(), lon.c_str());
        return false;
    }
    _lat = lat;
    _lon = lon;
    _hasGeo = true;
    _prefs.begin("weather", false);
    _prefs.putString("lat", _lat);
    _prefs.putString("lon", _lon);
    _prefs.end();
    Serial.printf("[%s] location set: %s, %s\n", TAG, _lat.c_str(), _lon.c_str());
    _nextFetchMs = millis();
    return true;
}

// Howard Hinnant's civil-from-days: interpret the LOCAL wall clock as if it
// were UTC. The clock's ForecastScene timestamp is exactly this.
int64_t Forecast::wallEpochNow() {
    struct tm t;
    if (!getLocalTime(&t, 0)) {
        return 0;
    }
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    int64_t d = t.tm_mday;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  // [0, 399]
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400LL + t.tm_hour * 3600LL + t.tm_min * 60LL + t.tm_sec;
}

bool Forecast::fetch(const char* unit, bool toScene) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[%s] no WiFi\n", TAG);
        return false;
    }
    if (!_hasGeo) {
        Serial.printf("[%s] no location set (geo <lat> <lon>)\n", TAG);
        return false;
    }
    if (_fetching) {
        return false;
    }
    _fetching = true;

    // Resolve + persist the unit preference (nullptr = stored).
    char u = _unit.length() ? _unit[0] : 'f';
    if (unit != nullptr && (unit[0] == 'c' || unit[0] == 'f') && unit[0] != u) {
        u = unit[0];
        _unit = String(u);
        _prefs.begin("weather", false);
        _prefs.putString("unit", _unit);
        _prefs.end();
        Serial.printf("[%s] unit set to %sF (persisted)\n", TAG, u == 'f' ? "" : "C/");
    }

    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + _lat +
                 "&longitude=" + _lon + "&hourly=temperature_2m&temperature_unit=" +
                 (u == 'f' ? "fahrenheit" : "celsius") +
                 "&forecast_days=2&timeformat=unixtime&timezone=auto";
    Serial.printf("[%s] fetching forecast...\n", TAG);

    bool ok = false;
    WiFiClientSecure tls;
    tls.setInsecure();  // no cert validation (community device class)
    tls.setTimeout(10);
    HTTPClient http;
    if (!http.begin(tls, url)) {
        Serial.printf("[%s] HTTP begin failed\n", TAG);
        _fetching = false;
        return false;
    }
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[%s] HTTP GET failed: %d\n", TAG, code);
        http.end();
        _fetching = false;
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.printf("[%s] JSON parse failed\n", TAG);
        _fetching = false;
        return false;
    }
    JsonArray times = doc["hourly"]["time"].as<JsonArray>();
    JsonArray temps = doc["hourly"]["temperature_2m"].as<JsonArray>();
    size_t n = times.size() < temps.size() ? times.size() : temps.size();
    if (n < 24) {
        Serial.printf("[%s] not enough hourly data (%u points)\n", TAG, (unsigned)n);
        _fetching = false;
        return false;
    }

    // Current local hour, matched against the API's local-epoch hour grid.
    int64_t nowHour = wallEpochNow();
    if (nowHour > 0) {
        nowHour -= nowHour % 3600;
    }
    size_t idx = 0;
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        int64_t t = times[i].as<int64_t>();
        if (t <= nowHour) {
            idx = i;
            found = true;
        } else {
            break;
        }
    }
    if (!found || idx + 24 > n) {
        Serial.printf("[%s] forecast does not cover the current hour\n", TAG);
        _fetching = false;
        return false;
    }

    ForecastScene fs = ForecastScene_init_default;
    fs.timestamp = times[idx].as<int64_t>();

    int32_t mn = INT32_MAX;
    int32_t mx = INT32_MIN;
    for (int i = 0; i < 24; i++) {
        long v = lround(temps[idx + i].as<double>());
        if (v < SHRT_MIN) v = SHRT_MIN;
        if (v > SHRT_MAX) v = SHRT_MAX;
        int16_t sv = (int16_t)v;
        fs.values.bytes[i * 2] = (uint8_t)(sv & 0xFF);
        fs.values.bytes[i * 2 + 1] = (uint8_t)((sv >> 8) & 0xFF);
        if (sv < mn) mn = sv;
        if (sv > mx) mx = sv;
    }
    fs.values.size = 48;
    fs.min = mn;
    fs.max = mx;
    fs.minColor = 0x0000FF;  // palette-mapped by the clock
    fs.maxColor = 0xFF0000;

    // [thermometer icon][value placeholder]°C / °F  (0xC2 prefixes an icon)
    static const uint8_t tmplC[] = {0xC2, 0x8F, 0x08, 0xC2, 0xB0, 'C'};
    static const uint8_t tmplF[] = {0xC2, 0x8F, 0x08, 0xC2, 0xB0, 'F'};
    const uint8_t* tmpl = u == 'f' ? tmplF : tmplC;
    memcpy(fs.templateText.bytes, tmpl, 6);
    fs.templateText.size = 6;

    uint8_t frame[128];
    size_t len = Glance::encodeForecastCommand(frame, sizeof(frame), &fs);
    if (len == 0) {
        Serial.printf("[%s] forecast encode failed\n", TAG);
        _fetching = false;
        return false;
    }
    Serial.printf("[%s] 24h forecast: %ld..%ld °%s (%s char, slot 1)\n", TAG, (long)mn,
                  (long)mx, u == 'f' ? "F" : "C", toScene ? "scene" : "data");
    ok = toScene ? _glance.sendSceneCommand(frame, len) : _glance.sendCommand(frame, len);
    if (ok && !toScene) {
        // Show the ring, then return to the watchface (see loop()).
        _hideAtMs = millis() + FORECAST_DISPLAY_MS;
        Serial.printf("[%s] forecast sent - showing for %us\n", TAG,
                      (unsigned)(FORECAST_DISPLAY_MS / 1000));
    }
    if (!ok) {
        Serial.printf("[%s] forecast NOT accepted%s\n", TAG,
                      toScene ? " (scene char len limit - use 'wx')"
                              : " (check connection)");
    }
    _fetching = false;
    return ok;
}
