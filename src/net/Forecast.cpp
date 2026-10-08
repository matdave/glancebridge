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

bool Forecast::fetch(const char* unit) {
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
                 "&longitude=" + _lon +
                 "&hourly=temperature_2m,precipitation_probability,wind_speed_10m,relative_humidity_2m" +
                 "&temperature_unit=" + (u == 'f' ? "fahrenheit" : "celsius") +
                 "&wind_speed_unit=" + (u == 'f' ? "mph" : "kmh") +
                 "&forecast_days=2&timeformat=unixtime&timezone=auto";
    Serial.printf("[%s] fetching forecast (heap=%u max-alloc=%u)...\n", TAG,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

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
    JsonArray rain = doc["hourly"]["precipitation_probability"].as<JsonArray>();
    JsonArray wind = doc["hourly"]["wind_speed_10m"].as<JsonArray>();
    JsonArray humid = doc["hourly"]["relative_humidity_2m"].as<JsonArray>();
    size_t n = times.size();
    if (temps.size() < n) n = temps.size();
    if (rain.size() < n) n = rain.size();
    if (wind.size() < n) n = wind.size();
    if (humid.size() < n) n = humid.size();
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
    int64_t ts = times[idx].as<int64_t>();

    // Ring captions (0xC2 prefixes an icon; 0x08 is the current-value
    // placeholder, per the ForecastScene proto). Icon ids from the C#
    // reference client: 143 thermometer, 149 rain, 152 wind, 144 humidity.
    static const uint8_t tmplC[] = {0xC2, 0x8F, 0x08, 0xC2, 0xB0, 'C'};
    static const uint8_t tmplF[] = {0xC2, 0x8F, 0x08, 0xC2, 0xB0, 'F'};
    static const uint8_t tmplRain[] = {0xC2, 0x95, 0x08, '%'};
    static const uint8_t tmplWind[] = {0xC2, 0x98, 0x08};
    static const uint8_t tmplHumid[] = {0xC2, 0x90, 0x08, '%'};
    const uint8_t* tmpl = u == 'f' ? tmplF : tmplC;

    // Temperature: auto-scaled to the data. The other rings use the fixed
    // scale the C# reference applies (0..100 % for rain/humidity, wind
    // auto-scaled from 0 to the day's max). Each ring is sent regardless of
    // the others' results (they live in independent carousel slots).
    ok = sendRing(temps, idx, ts, FORECAST_SLOT, 0x0000FF, 0xFF0000,
                  INT32_MIN, INT32_MIN, tmpl, 6, "temp");
    ok = sendRing(rain, idx, ts, RAIN_SLOT, 0x0000FF, 0x00BFFF,
                  0, 100, tmplRain, sizeof(tmplRain), "rain") && ok;
    ok = sendRing(wind, idx, ts, WIND_SLOT, 0x00FF00, 0xFF0000,
                  0, INT32_MIN, tmplWind, sizeof(tmplWind), "wind") && ok;
    ok = sendRing(humid, idx, ts, HUMIDITY_SLOT, 0x00FFFF, 0x0000FF,
                  0, 100, tmplHumid, sizeof(tmplHumid), "humidity") && ok;
    if (ok) {
        Serial.printf("[%s] weather rings sent (temp/rain/wind/humidity)\n", TAG);
    } else if (WiFi.status() == WL_CONNECTED && !_glance.isConnected()) {
        // Clock not up yet (boot): retry soon instead of waiting 30 min.
        _nextFetchMs = millis() + 60000;
    }
    _fetching = false;
    Serial.printf("[%s] fetch done (ok=%d heap=%u max-alloc=%u)\n", TAG, ok ? 1 : 0,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return ok;
}

bool Forecast::sendRing(const JsonArray& values, size_t idx, int64_t ts,
                        uint8_t slot, int32_t minColor, int32_t maxColor,
                        int32_t ringMin, int32_t ringMax,
                        const uint8_t* tmpl, size_t tmplLen, const char* label) {
    if (idx + 24 > values.size()) {
        Serial.printf("[%s] %s: not enough data\n", TAG, label);
        return false;
    }

    ForecastScene fs = ForecastScene_init_default;
    fs.timestamp = ts;

    int32_t mn = INT32_MAX;
    int32_t mx = INT32_MIN;
    for (int i = 0; i < 24; i++) {
        long v = lround(values[idx + i].as<double>());
        if (v < SHRT_MIN) v = SHRT_MIN;
        if (v > SHRT_MAX) v = SHRT_MAX;
        int16_t sv = (int16_t)v;
        fs.values.bytes[i * 2] = (uint8_t)(sv & 0xFF);
        fs.values.bytes[i * 2 + 1] = (uint8_t)((sv >> 8) & 0xFF);
        if (sv < mn) mn = sv;
        if (sv > mx) mx = sv;
    }
    fs.values.size = 48;
    // Ring scale: a fixed ringMin/ringMax wins; otherwise auto-scale the
    // corresponding edge from the data (INT32_MIN = auto).
    fs.min = ringMin != INT32_MIN ? ringMin : mn;
    fs.max = ringMax != INT32_MIN ? ringMax : mx;
    // A flat ring (min == max) renders as an empty circle. Wind auto-scale
    // (0..day-max) floors the max at 1 like the C# reference client.
    if (ringMax == INT32_MIN && ringMin == 0 && fs.max < 1) {
        fs.max = 1;
    }
    fs.minColor = minColor;  // palette-mapped by the clock
    fs.maxColor = maxColor;

    memcpy(fs.templateText.bytes, tmpl, tmplLen);
    fs.templateText.size = (pb_size_t)tmplLen;

    uint8_t frame[128];
    size_t len = Glance::encodeForecastCommand(frame, sizeof(frame), &fs, slot);
    if (len == 0) {
        Serial.printf("[%s] %s encode failed\n", TAG, label);
        return false;
    }
    bool ok = _glance.sendCommand(frame, len);
    Serial.printf("[%s] %s: %ld..%ld (slot %u) %s\n", TAG, label, (long)fs.min,
                  (long)fs.max, (unsigned)slot, ok ? "sent" : "rejected");
    return ok;
}
