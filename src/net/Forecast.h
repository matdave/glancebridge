#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include "glance/GlanceClient.h"

// 24h weather rings for the clock (temperature, rain, wind, humidity)
// from Open-Meteo (no API key).
//
//   geo <lat> <lon>   store location (NVS), scheduled fetch follows
//   wx [f]            fetch + display now (optional fahrenheit)
//
// Re-fetches every 30 minutes while WiFi is up. Each metric is a
// ForecastScene pushed to its own carousel slot via cmd 7 (see
// GlanceCore); the proto's timestamp is "localtime encoded as epoch" and
// Open-Meteo's timeformat=unixtime with timezone=auto returns exactly
// that, so the API's hour value is used directly.
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
    // "c" or "f" — an explicit choice is persisted. Pushes all four weather
    // rings (temperature, rain, wind, humidity) to their carousel slots.
    bool fetch(const char* unit = nullptr);

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

    // Build a ForecastScene for one metric series (24 points from `values`
    // at offset `idx`) and send it to the given carousel slot. ringMin /
    // ringMax are the ring scale; a value of INT32_MIN means "auto from the
    // data" (min or max independently). Returns true when the clock accepted.
    bool sendRing(const JsonArray& values, size_t idx, int64_t ts, uint8_t slot,
                  int32_t minColor, int32_t maxColor,
                  int32_t ringMin, int32_t ringMax,
                  const uint8_t* tmpl, size_t tmplLen, const char* label);

    // Carousel slots the weather rings are pushed to (frame byte b). The
    // FACTORY carousel layout (clock fw 1.5) is: slot 0 = empty (renders
    // dim), slot 1 = the built-in digital watchface. Community clients use
    // slot 1 for temperature because they install their own digital-time
    // scene at slot 0 first - on this clock that would OVERWRITE the
    // factory watchface. So the rings occupy slots 2+ and leave the factory
    // slots untouched (matches the C# reference layout, shifted by one).
    static constexpr uint8_t FORECAST_SLOT = 2;   // temperature
    static constexpr uint8_t RAIN_SLOT = 3;
    static constexpr uint8_t WIND_SLOT = 4;
    static constexpr uint8_t HUMIDITY_SLOT = 5;
    // Display duration before deleting the scene (ScenesDelete cmd 33).
    // 0 = keep the scene in the carousel (the clock may return to the
    // watchface on its own; was previously needed because the forecast
    // overwrote the factory watchface slot).
    static constexpr uint32_t FORECAST_DISPLAY_MS = 0;

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
