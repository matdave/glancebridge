#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "glance/GlanceClient.h"

// 24h weather rings for the clock (temperature, rain, wind, humidity).
// Two providers, both without an API key:
//   om   Open-Meteo (worldwide; default)
//   nws  US National Weather Service, api.weather.gov (US locations only)
// Switch with `wxapi nws|om` (serial) or the /weather portal page; the
// choice is stored in NVS.
//
//   geo <lat> <lon>   store location (NVS), scheduled fetch follows
//   wx [f]            fetch + display now (optional fahrenheit)
//
// Re-fetches every 30 minutes while WiFi is up. Each metric is a
// ForecastScene pushed to its own carousel slot via cmd 7 (see
// GlanceCore); the proto's timestamp is "localtime encoded as epoch".
// Open-Meteo's timeformat=unixtime with timezone=auto returns exactly
// that, and NWS startTime components are used the same way.
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
    String provider() const { return _provider; }

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

    // Switch provider: "om" (Open-Meteo) or "nws" (US National Weather
    // Service). Persisted; schedules an immediate refetch. Returns false
    // for anything else.
    bool setProvider(const String& p);

private:
    // Provider-normalized hourly series: 24 points starting at the current
    // local hour. Both providers fill this; sendRings() is provider-free.
    struct Hourly {
        int64_t ts = 0;         // clock epoch of the first point (this hour)
        float temp[24] = {0};
        float rain[24] = {0};   // precipitation probability %
        float wind[24] = {0};
        float humid[24] = {0};  // relative humidity %
    };

    // Current local wall time encoded as epoch (what the clock expects).
    static int64_t wallEpochNow();

    bool fetchOpenMeteo(char u, Hourly& out);
    bool fetchNws(char u, Hourly& out);

    // Push all four rings from a filled Hourly (shared by both providers).
    bool sendRings(const Hourly& h, char u);

    // Build a ForecastScene for one 24-point metric series and send it to
    // the given carousel slot. ringMin / ringMax are the ring scale; a
    // value of INT32_MIN means "auto from the data" (min or max
    // independently). Returns true when the clock accepted.
    bool sendRing(int64_t ts, const float* values, uint8_t slot,
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
    String _unit = "f";       // persisted; 'c' switches to celsius
    String _provider = "om";  // persisted: "om" or "nws"
    bool _hasGeo = false;
    bool _fetching = false;
    uint32_t _nextFetchMs = 0;
    uint32_t _hideAtMs = 0;  // 0 = not showing
};
