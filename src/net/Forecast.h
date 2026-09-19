#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include "glance/GlanceClient.h"

// 24h temperature forecast ring for the clock, from Open-Meteo (no API key).
//
//   geo <lat> <lon>   store location (NVS), scheduled fetch follows
//   wx [f]            fetch + display now (optional fahrenheit)
//
// Re-fetches every 30 minutes while WiFi is up. The clock frame is
// [7,16,24,1] + ForecastScene (see GlanceCore); the proto's timestamp is
// "localtime encoded as epoch" and Open-Meteo's timeformat=unixtime with
// timezone=auto returns exactly that, so the API's hour value is used
// directly.
//
// Named Forecast (not Weather): ChronosESP32.h defines struct Weather.

class Forecast {
public:
    explicit Forecast(GlanceClient& glance) : _glance(glance) {}

    void begin();  // load stored location from NVS
    void loop();   // periodic refresh (30 min) when WiFi is connected

    // Store the location (decimal degrees) and schedule an immediate fetch.
    bool setLocation(const String& lat, const String& lon);

    // Blocking fetch + send. unit: nullptr = stored preference (default F);
    // "c" or "f" — an explicit choice is persisted. toScene: write the frame
    // to the Scene characteristic instead of the data characteristic
    // (probe for firmware rejecting cmd 7 on the data path).
    bool fetch(const char* unit = nullptr, bool toScene = false);

    bool hasLocation() const { return _hasGeo; }
    String location() const { return _lat + ", " + _lon; }
    String lat() const { return _lat; }
    String lon() const { return _lon; }
    String unit() const { return _unit; }

    // Persist the unit preference ("c"/"f") without fetching.
    void setUnit(const String& u) {
        if (u != "c" && u != "f") {
            return;
        }
        _unit = u;
        _prefs.begin("weather", false);
        _prefs.putString("unit", _unit);
        _prefs.end();
    }

private:
    // Current local wall time encoded as epoch (what the clock expects).
    static int64_t wallEpochNow();

    // Carousel slot the forecast scene is pushed to (frame byte b).
    static constexpr uint8_t FORECAST_SLOT = 1;
    // Display duration before deleting the scene (ScenesDelete cmd 33),
    // returning the clock to its native watchface.
    static constexpr uint32_t FORECAST_DISPLAY_MS = 15000;

    GlanceClient& _glance;
    Preferences _prefs;
    String _lat;
    String _lon;
    String _unit = "f";  // persisted; 'c' switches to celsius
    bool _hasGeo = false;
    bool _fetching = false;
    uint32_t _nextFetchMs = 0;
    uint32_t _hideAtMs = 0;  // 0 = not showing
};
