#pragma once

#include <Arduino.h>
#include <ChronosESP32.h>

#include <functional>

#include <glance/GlanceClient.h>

// Bridges the Chronos phone app (BLE peripheral side) to the Glance clock
// (central side).
//
// - Relays phone notifications as clock Notify commands
// - Phone time sync (CF_TIME) sets the ESP32 system clock via settimeofday;
//   the Current Time Service then serves that time to the clock hands
//
// The Chronos BLE server cannot be stopped without deiniting NimBLE (which
// would kill the central connection to the clock), so setRelay(false) only
// pauses the relay; the peripheral keeps running once started.

class ChronosBridge {
public:
    explicit ChronosBridge(GlanceClient& glance) : _glance(glance) {}

    void begin();  // starts the Chronos peripheral; call once
    void loop();   // must be called regularly

    void setRelay(bool on) { _relay = on; }
    bool relayEnabled() const { return _relay; }
    bool started() const { return _started; }
    bool phoneConnected() { return _started && _watch.isConnected(); }

    // Set the policy hook for phone time pushes (called after the library
    // has already applied the time to the system clock).
    void onPhoneTime(std::function<void()> hook) { _onPhoneTime = std::move(hook); }

    // Push the Chronos app's alarms to the clock as an Alarms protobuf
    // (auto-runs on the main loop when the app changes an alarm).
    void pushAlarms();
    void printAlarms();

    // Queue a notice for the main-loop relay (host-task safe: fixed buffer
    // + flag, no blocking work in NimBLE callbacks).
    void queueNotice(const String& text);

private:
    void onNotification(const Notification& n);
    void relayNotice(const String& text);

    GlanceClient& _glance;
    ChronosESP32 _watch{"GlanceBridge"};
    bool _started = false;
    bool _relay = true;
    std::function<void()> _onPhoneTime;

    // Pending work from host-task callbacks, drained by loop().
    volatile bool _phoneTimePending = false;
    volatile bool _alarmPushPending = false;
    volatile bool _noticePending = false;
    char _noticeBuf[160] = {0};
};
