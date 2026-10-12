#include <net/Forecast.h>

#include <WiFiClientSecure.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>

static const char* TAG = "Weather";

// Howard Hinnant's days_from_civil: encode LOCAL wall-clock components as
// if they were UTC - exactly the timestamp encoding the clock's
// ForecastScene expects.
static int64_t civilEpoch(int y, int m, int d, int h, int mi, int s) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  // [0, 399]
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400LL + h * 3600LL + mi * 60LL + s;
}

static float toC(float f) { return (f - 32.0f) * 5.0f / 9.0f; }
static float toF(float c) { return c * 9.0f / 5.0f + 32.0f; }

// NWS windSpeed is prose: "10 mph", "5 to 10 mph", "15 km/h". The first
// number is used, converted to the ring's unit system.
static float nwsWind(const char* s, bool metric) {
    const char* p = s;
    while (*p && (*p < '0' || *p > '9')) {
        p++;
    }
    float v = strtof(p, nullptr);
    bool kmh = strstr(s, "km/h") != nullptr;
    if (metric) {
        return kmh ? v : v * 1.609344f;
    }
    return kmh ? v / 1.609344f : v;
}

// NWS startTime: "2026-10-12T15:00:00-05:00". Only the local wall-clock
// components matter (localtime-as-epoch, see civilEpoch); the UTC offset
// is ignored.
static int64_t nwsStartEpoch(const char* s) {
    int y, mo, d, h, mi;
    if (sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
        return 0;
    }
    return civilEpoch(y, mo, d, h, mi, 0);
}

// Minimal HTTP chunked-transfer decoder as an ArduinoJson input stream.
//
// HTTPClient::getStreamPtr() exposes the RAW socket stream: with
// Transfer-Encoding: chunked (api.weather.gov serves the ~160 KB hourly
// forecast that way over HTTP/1.1) the JSON body is framed as
//   <size hex>[;ext]\r\n <data> \r\n ... 0\r\n\r\n
// and a plain stream parse reads the chunk size as the document root -
// ArduinoJson returns Ok with an integer document (-> 0 periods). The
// points lookup only worked because getString() decodes chunks (and
// cannot hold the decoded body in RAM anyway). This wrapper strips the
// framing on the fly and auto-detects the encoding: a plain JSON document
// starts with '{' or '[', a chunked one with the hex size line.
//
// Empty reads are NOT end-of-stream: NetworkClientSecure::read() returns
// -1 (not 0) when nothing has been decrypted YET, and its readBytes()
// bails on the same condition. Every read below is therefore gated on
// available() - the pattern HTTPClient's own body loop uses - and only a
// closed socket or the deadline ends the stream. Conflating "no data
// right now" with EOF truncated the body (ArduinoJson IncompleteInput).
class ChunkedStream : public Stream {
public:
    explicit ChunkedStream(Client& src) : _src(src) {}

    int available() override {
        if (_pos >= _len && !fill()) {
            return 0;
        }
        return _len - _pos;
    }

    int read() override {
        if (_pos >= _len && !fill()) {
            return -1;
        }
        _decoded++;
        return (unsigned char)_buf[_pos++];
    }

    int peek() override {
        if (_pos >= _len && !fill()) {
            return -1;
        }
        return (unsigned char)_buf[_pos];
    }

    // Input-only stream (Stream inherits Print's pure virtual write).
    size_t write(uint8_t) override { return 0; }

    // Diagnostic: decoded body bytes handed to the parser so far.
    size_t decoded() const { return _decoded; }

private:
    static constexpr uint32_t READ_TIMEOUT_MS = 10000;

    // Refill the buffer with decoded body bytes; false at end of body.
    bool fill() {
        if (_done) {
            return false;
        }
        if (!_detected) {
            int c = skipWs();
            if (c < 0) {
                _done = true;
                return false;
            }
            _detected = true;
            if (c == '{' || c == '[') {
                _chunked = false;
                _buf[0] = (uint8_t)c;
                _pos = 0;
                _len = 1;
                return true;
            }
            _chunked = true;
            _pending = c;  // first digit of the first chunk-size line
        }
        _pos = 0;
        _len = 0;
        size_t want = sizeof(_buf);
        if (_chunked) {
            if (_chunkRemain == 0) {
                long size = readChunkSize();
                if (size <= 0) {  // 0 = the terminating chunk
                    _done = true;
                    return false;
                }
                _chunkRemain = size;
            }
            if ((size_t)_chunkRemain < want) {
                want = (size_t)_chunkRemain;
            }
        }
        _len = pull(_buf, want);
        if (_len == 0) {  // socket closed or stalled
            _done = true;
            return false;
        }
        if (_chunked) {
            _chunkRemain -= _len;
            if (_chunkRemain == 0) {
                uint8_t cr, lf;  // the CRLF terminating every chunk
                nextByte(cr);
                nextByte(lf);
            }
        }
        return true;
    }

    // Up to `want` bytes, waiting out empty reads. 0 = EOF/timeout.
    size_t pull(uint8_t* dst, size_t want) {
        uint32_t deadline = millis() + READ_TIMEOUT_MS;
        while (true) {
            if (_src.available() > 0) {
                int r = _src.read(dst, want);
                if (r > 0) {
                    return (size_t)r;
                }
                if (r < 0 && !_src.connected()) {
                    return 0;  // socket closed / error
                }
            }
            if (!_src.connected()) {
                return 0;  // peer closed
            }
            if ((int32_t)(millis() - deadline) >= 0) {
                return 0;  // stalled
            }
            delay(2);
        }
    }

    // One raw byte, waiting out empty reads. False at EOF/timeout.
    bool nextByte(uint8_t& out) {
        uint32_t deadline = millis() + READ_TIMEOUT_MS;
        while (true) {
            if (_src.available() > 0 && _src.read(&out, 1) == 1) {
                return true;
            }
            if (!_src.connected()) {
                return false;
            }
            if ((int32_t)(millis() - deadline) >= 0) {
                return false;
            }
            delay(2);
        }
    }

    int skipWs() {
        uint8_t c;
        while (nextByte(c)) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                return c;
            }
        }
        return -1;
    }

    // Chunk-size line: hex digits, optional ";extension", ends at LF.
    long readChunkSize() {
        long v = 0;
        bool any = false;
        int c;
        if (_pending >= 0) {
            c = _pending;
            _pending = -1;
        } else {
            uint8_t b;
            if (!nextByte(b)) {
                return -1;
            }
            c = b;
        }
        while (true) {
            if (c >= '0' && c <= '9') {
                v = v * 16 + (c - '0');
                any = true;
            } else if (c >= 'a' && c <= 'f') {
                v = v * 16 + (c - 'a' + 10);
                any = true;
            } else if (c >= 'A' && c <= 'F') {
                v = v * 16 + (c - 'A' + 10);
                any = true;
            } else if (c == '\n') {
                return any ? v : -1;
            } else if (c == ';') {  // chunk extension: skip to end of line
                uint8_t b;
                while (nextByte(b) && b != '\n') {
                }
                return any ? v : -1;
            }
            uint8_t b;
            if (!nextByte(b)) {
                return -1;
            }
            c = b;
        }
    }

    Client& _src;
    uint8_t _buf[256];
    size_t _pos = 0;
    size_t _len = 0;
    long _chunkRemain = 0;
    int _pending = -1;
    bool _detected = false;
    bool _chunked = false;
    bool _done = false;
    size_t _decoded = 0;
};

// Streams the elements of the NWS "periods" array out of the decoded JSON,
// one object at a time. Parsing the whole 156-period document needs ~53 KB
// of ArduinoJson pool (measured; every kept key/string is copied when the
// input is a stream), which NoMemories the ESP32 - and only 24 periods are
// ever used, so each element is parsed into its own tiny JsonDocument
// instead. Wrap the raw socket stream in ChunkedStream first.
class PeriodScanner {
public:
    explicit PeriodScanner(Stream& src) : _src(src) {}

    // Copy the next period object into buf, NUL-terminated. 0 = end of
    // array / input failure / element larger than cap (skipped).
    size_t next(char* buf, size_t cap) {
        if (_done) {
            return 0;
        }
        if (!_inArray && !enterArray()) {
            _done = true;
            return 0;
        }
        _inArray = true;
        int c;
        while ((c = _src.read()) >= 0 && c != '{' && c != ']') {
        }
        if (c != '{') {  // array ended (']') or input failed
            _done = true;
            return 0;
        }
        // Copy the object verbatim, tracking nesting and string state.
        size_t n = 0;
        bool inStr = false, esc = false;
        int depth = 0;
        while (c >= 0) {
            if (n < cap) {
                buf[n++] = (char)c;
            }
            if (inStr) {
                if (esc) {
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    inStr = false;
                }
            } else if (c == '"') {
                inStr = true;
            } else if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    break;
                }
            }
            c = _src.read();
        }
        if (c < 0 || n >= cap) {
            _done = true;
            return 0;
        }
        buf[n] = '\0';
        return n;
    }

private:
    // Advance past the "periods" key to the start of its array.
    bool enterArray() {
        static const char KEY[] = "\"periods\"";
        size_t m = 0;
        int c;
        while ((c = _src.read()) >= 0) {
            if (c == KEY[m]) {
                if (KEY[++m] == '\0') {
                    do {  // skip whitespace, expect ':' then '['
                        c = _src.read();
                    } while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
                    if (c != ':') {
                        return false;
                    }
                    do {
                        c = _src.read();
                    } while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
                    return c == '[';
                }
            } else {
                m = (c == KEY[0]) ? 1 : 0;
            }
        }
        return false;
    }

    Stream& _src;
    bool _inArray = false;
    bool _done = false;
};

void Forecast::begin() {
    // Writable open creates the namespace so later read-only opens don't
    // log "nvs_open failed: NOT_FOUND" on fresh installs (before 'geo').
    _prefs.begin("weather", false);
    _lat = _prefs.getString("lat", "");
    _lon = _prefs.getString("lon", "");
    _unit = _prefs.getString("unit", "f");
    _provider = _prefs.getString("provider", "om");
    if (_provider != "nws") {
        _provider = "om";
    }
    _prefs.end();
    _hasGeo = _lat.length() > 0 && _lon.length() > 0;
    if (_hasGeo) {
        Serial.printf("[%s] stored location: %s, %s (unit %s, provider %s)\n",
                      TAG, _lat.c_str(), _lon.c_str(), _unit == "f" ? "F" : "C",
                      _provider.c_str());
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

bool Forecast::setProvider(const String& p) {
    String v = p;
    v.trim();
    v.toLowerCase();
    if (v != "om" && v != "nws") {
        return false;
    }
    _provider = v;
    _prefs.begin("weather", false);
    _prefs.putString("provider", _provider);
    _prefs.end();
    Serial.printf("[%s] provider set: %s\n", TAG, v.c_str());
    _nextFetchMs = millis();  // refetch from the new provider
    return true;
}

// Current local wall time encoded as epoch (what the clock expects).
int64_t Forecast::wallEpochNow() {
    struct tm t;
    if (!getLocalTime(&t, 0)) {
        return 0;
    }
    return civilEpoch(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                      t.tm_min, t.tm_sec);
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

    Serial.printf("[%s] fetching forecast via %s (heap=%u max-alloc=%u)...\n", TAG,
                  _provider == "nws" ? "nws" : "open-meteo",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    Hourly h;
    bool ok = _provider == "nws" ? fetchNws(u, h) : fetchOpenMeteo(u, h);
    if (!ok) {
        _fetching = false;
        return false;
    }

    bool sent = sendRings(h, u);
    if (sent) {
        Serial.printf("[%s] weather rings sent (temp/rain/wind/humidity)\n", TAG);
    } else if (WiFi.status() == WL_CONNECTED && !_glance.isConnected()) {
        // Clock not up yet (boot): retry soon instead of waiting 30 min.
        _nextFetchMs = millis() + 60000;
    }
    _fetching = false;
    Serial.printf("[%s] fetch done (ok=%d heap=%u max-alloc=%u)\n", TAG,
                  sent ? 1 : 0, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    return sent;
}

bool Forecast::fetchOpenMeteo(char u, Hourly& out) {
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + _lat +
                 "&longitude=" + _lon +
                 "&hourly=temperature_2m,precipitation_probability,wind_speed_10m,relative_humidity_2m" +
                 "&temperature_unit=" + (u == 'f' ? "fahrenheit" : "celsius") +
                 "&wind_speed_unit=" + (u == 'f' ? "mph" : "kmh") +
                 "&forecast_days=2&timeformat=unixtime&timezone=auto";

    WiFiClientSecure tls;
    tls.setInsecure();  // no cert validation (community device class)
    tls.setTimeout(10000);  // ms - Stream timeouts are milliseconds
    HTTPClient http;
    if (!http.begin(tls, url)) {
        Serial.printf("[%s] HTTP begin failed\n", TAG);
        return false;
    }
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[%s] HTTP GET failed: %d\n", TAG, code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.printf("[%s] JSON parse failed\n", TAG);
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
        return false;
    }

    out.ts = times[idx].as<int64_t>();
    for (int i = 0; i < 24; i++) {
        out.temp[i] = temps[idx + i].as<double>();
        out.rain[i] = rain[idx + i].as<double>();
        out.wind[i] = wind[idx + i].as<double>();
        out.humid[i] = humid[idx + i].as<double>();
    }
    return true;
}

bool Forecast::fetchNws(char u, Hourly& out) {
    WiFiClientSecure tls;
    tls.setInsecure();  // no cert validation (community device class)
    tls.setTimeout(10000);  // ms - Stream timeouts are milliseconds
    HTTPClient http;
    // api.weather.gov policy: an identifying User-Agent is required.
    http.setUserAgent("glanceclock-bridge/1.0 (local ESP32 bridge)");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    // Step 1: location -> gridpoint hourly forecast URL.
    String url = "https://api.weather.gov/points/" + _lat + "," + _lon;
    if (!http.begin(tls, url)) {
        Serial.printf("[%s] HTTP begin failed\n", TAG);
        return false;
    }
    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[%s] points lookup failed: %d (NWS covers US locations "
                      "only)\n", TAG, code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.printf("[%s] points JSON parse failed\n", TAG);
        return false;
    }
    const char* hourlyUrl = doc["properties"]["forecastHourly"] | "";
    if (!hourlyUrl[0]) {
        Serial.printf("[%s] no hourly forecast URL in points response\n", TAG);
        return false;
    }

    // Step 2: the hourly forecast. Streamed with a filter - the raw
    // response (~160 KB for 156 periods) does not fit the heap as a String.
    if (!http.begin(tls, hourlyUrl)) {
        Serial.printf("[%s] HTTP begin failed (hourly)\n", TAG);
        return false;
    }
    code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[%s] hourly GET failed: %d\n", TAG, code);
        http.end();
        return false;
    }

    // The whole hourly document is ~160 KB / 156 periods and needs ~53 KB
    // of parser pool (measured) - too much for this heap - so the periods
    // array is consumed one element at a time and parsing stops as soon as
    // 24 hourly points are collected. The first period is the anchor: NWS
    // hourly periods start at the top of an hour, so its startTime is the
    // first hour of the ring (one hour of slack for a mid-hour edge case).
    JsonDocument filter;
    // Period shape (api.weather.gov forecast/hourly): temperature is a
    // bare number with a separate "F"/"C" temperatureUnit; rain/humidity
    // are {unitCode, value} objects; windSpeed is prose ("15 mph").
    filter["startTime"] = true;
    filter["temperature"] = true;
    filter["temperatureUnit"] = true;
    filter["probabilityOfPrecipitation"]["value"] = true;
    filter["windSpeed"] = true;
    filter["relativeHumidity"]["value"] = true;

    ChunkedStream bodyStream(*http.getStreamPtr());
    PeriodScanner scanner(bodyStream);
    static char elem[4096];  // one period; observed elements are < 1 KB
    JsonDocument per;

    int64_t nowHour = wallEpochNow();
    if (nowHour > 0) {
        nowHour -= nowHour % 3600;
    }
    bool metric = u == 'c';
    int got = 0;
    size_t len;
    while (got < 24 && (len = scanner.next(elem, sizeof(elem))) != 0) {
        per.clear();
        if (deserializeJson(per, elem, len, DeserializationOption::Filter(filter))) {
            continue;  // malformed element - skip it
        }
        if (got == 0) {
            int64_t t0 = nwsStartEpoch(per["startTime"] | "");
            if (t0 <= 0 ||
                (nowHour > 0 && (t0 < nowHour - 3600 || t0 > nowHour + 3600))) {
                Serial.printf("[%s] unexpected first period '%s'\n", TAG,
                              (const char*)(per["startTime"] | "?"));
                http.end();
                return false;
            }
            out.ts = t0;
        }
        // NWS reports temperature with its own unit per period ("F"/"C")
        // and wind speeds as prose with their own unit - normalized here.
        double t = per["temperature"] | 0.0;
        const char* tUnit = per["temperatureUnit"] | "F";
        bool degC = tUnit[0] == 'C' || tUnit[0] == 'c';
        out.temp[got] = metric ? (degC ? (float)t : toC((float)t))
                               : (degC ? toF((float)t) : (float)t);
        out.rain[got] = per["probabilityOfPrecipitation"]["value"] | 0.0;
        out.humid[got] = per["relativeHumidity"]["value"] | 0.0;
        out.wind[got] = nwsWind(per["windSpeed"] | "", metric);
        got++;
    }
    http.end();
    if (got < 24) {
        Serial.printf("[%s] not enough hourly periods (%d, decoded %u bytes)\n",
                      TAG, got, (unsigned)bodyStream.decoded());
        return false;
    }
    return true;
}

bool Forecast::sendRings(const Hourly& h, char u) {
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
    bool ok = sendRing(h.ts, h.temp, FORECAST_SLOT, 0x0000FF, 0xFF0000,
                       INT32_MIN, INT32_MIN, tmpl, 6, "temp");
    ok = sendRing(h.ts, h.rain, RAIN_SLOT, 0x0000FF, 0x00BFFF,
                  0, 100, tmplRain, sizeof(tmplRain), "rain") && ok;
    ok = sendRing(h.ts, h.wind, WIND_SLOT, 0x00FF00, 0xFF0000,
                  0, INT32_MIN, tmplWind, sizeof(tmplWind), "wind") && ok;
    ok = sendRing(h.ts, h.humid, HUMIDITY_SLOT, 0x00FFFF, 0x0000FF,
                  0, 100, tmplHumid, sizeof(tmplHumid), "humidity") && ok;
    return ok;
}

bool Forecast::sendRing(int64_t ts, const float* values, uint8_t slot,
                        int32_t minColor, int32_t maxColor,
                        int32_t ringMin, int32_t ringMax,
                        const uint8_t* tmpl, size_t tmplLen, const char* label) {
    ForecastScene fs = ForecastScene_init_default;
    fs.timestamp = ts;

    int32_t mn = INT32_MAX;
    int32_t mx = INT32_MIN;
    for (int i = 0; i < 24; i++) {
        long v = lround(values[i]);
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
