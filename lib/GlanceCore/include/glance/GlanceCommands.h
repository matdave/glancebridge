#pragma once

#include <stddef.h>
#include <stdint.h>

// Glance Clock BLE protocol constants.
// Source: https://github.com/Hypfer/glance-clock (WTFPL, see proto/LICENSE.txt)

namespace Glance {

static constexpr const char* SERVICE_UUID = "5075f606-1e0e-11e7-93ae-92361f002671";
static constexpr const char* DATA_CHAR_UUID = "5075fb2e-1e0e-11e7-93ae-92361f002671";

// Reading DATA_CHAR returns a Settings message, writing executes a command.
// Command frame: [type, prio, arg1, arg2] + protobuf payload.
// Trailing zero bytes of the header may be omitted by the sender; we always
// send the full 4-byte header.

namespace Cmd {
constexpr uint8_t CustomScene = 0;
constexpr uint8_t Notify = 2;
constexpr uint8_t Timer = 3;
constexpr uint8_t Alarm = 4;
constexpr uint8_t Settings = 5;
constexpr uint8_t CallScene = 6;
constexpr uint8_t SaveForecastScene = 7;
constexpr uint8_t SaveAppointmentsScene = 8;
constexpr uint8_t TimerStop = 10;
constexpr uint8_t AlarmStop = 20;
constexpr uint8_t AlarmClear = 21;
constexpr uint8_t ScenesStop = 30;
constexpr uint8_t ScenesStart = 31;
constexpr uint8_t ScenesClear = 32;
constexpr uint8_t ScenesDelete = 33;
constexpr uint8_t ScenesDeleteMany = 34;
constexpr uint8_t UpdateAndRefresh = 35;
constexpr uint8_t EnableAutomaticNightMode = 40;
constexpr uint8_t DisableAutomaticNightMode = 41;
constexpr uint8_t BondsClear = 42;
constexpr uint8_t StartCalibration = 43;
constexpr uint8_t ConfirmCalibration = 44;
constexpr uint8_t ClearUserInfo = 50;
constexpr uint8_t BrightnessSceneStop = 60;
constexpr uint8_t BrightnessSceneStart = 61;
constexpr uint8_t DspStateShow = 70;
}  // namespace Cmd

namespace ScenePriority {
constexpr uint8_t BandLow = 1;
constexpr uint8_t SystemIdle = 1;
constexpr uint8_t BandMedium = 16;
constexpr uint8_t BandSystem = 32;
constexpr uint8_t SystemMsg = 33;
constexpr uint8_t BandHigh = 48;
constexpr uint8_t BandHighest = 64;
constexpr uint8_t BandCritical = 80;
}  // namespace ScenePriority

namespace TaskPriority {
constexpr uint8_t Timer = 46;
constexpr uint8_t Alarm = 47;
constexpr uint8_t TimerEnd = 80;
constexpr uint8_t PwrState = 81;
constexpr uint8_t Call = 83;
constexpr uint8_t PinCode = 90;
constexpr uint8_t Greetings = 255;
}  // namespace TaskPriority

// Write the 4-byte command header. Returns 4.
inline size_t makeHeader(uint8_t* out, uint8_t type, uint8_t prio, uint8_t a = 0,
                         uint8_t b = 0) {
    out[0] = type;
    out[1] = prio;
    out[2] = a;
    out[3] = b;
    return 4;
}

// Append the 4-byte command header in front of a protobuf payload.
// out must have room for 4 + payloadLen bytes.
inline size_t makeCommand(uint8_t* out, size_t cap, uint8_t type, uint8_t prio, uint8_t a,
                          uint8_t b, const uint8_t* payload, size_t payloadLen) {
    size_t total = 4 + payloadLen;
    if (cap < total) {
        return 0;
    }
    makeHeader(out, type, prio, a, b);
    for (size_t i = 0; i < payloadLen; i++) {
        out[4 + i] = payload[i];
    }
    return total;
}

}  // namespace Glance
