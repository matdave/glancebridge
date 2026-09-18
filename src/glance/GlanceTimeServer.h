#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

// Current Time Service (0x1805 / characteristic 0x2A2B).
//
// Per the protocol docs, the Glance clock polls a Current Time Service on
// the central device after connecting and uses it to set its hands.
// The ESP32 runs this GATT server while also acting as central to the
// clock (both roles on one NimBLE host).
//
// The characteristic holds 10 bytes per the Bluetooth spec:
//   year (u16 LE), month (1-12), day, hours, minutes, seconds,
//   day of week (1=Mon..7=Sun), fractions of 1/256s, adjust reason.
//
// Time is local wall-clock time (the clock has no timezone handling).

class GlanceTimeServer {
public:
    void begin();  // idempotent; call after NimBLEDevice::init
    void loop();   // refreshes the published time every second

private:
    void refresh();
    NimBLECharacteristic* _currentTime = nullptr;
    bool _started = false;
    uint32_t _nextRefreshMs = 0;
};
