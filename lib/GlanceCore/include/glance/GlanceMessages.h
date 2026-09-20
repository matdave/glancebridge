#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Glance.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "GlanceCommands.h"

// High-level builders for Glance Clock protocol messages.
// All functions return the number of bytes written to `out`,
// or 0 if the buffer is too small / encoding failed.

namespace Glance {

// Encode a Notice protobuf message (payload only, no command header).
// Passing the zero value of an optional field (Animation_NoneAnimation,
// Sound_NoneSound, Color_Black) omits the field so the clock applies its
// default (Pulse / Radar / Lime).
size_t encodeNotice(uint8_t* out, size_t cap, const char* text,
                    Animation anim = Animation_NoneAnimation,
                    Sound sound = Sound_NoneSound, Color color = Color_Black,
                    TextData_Modificator mod = TextData_Modificator_ModNone);

// Encode a full Notify command (header + Notice payload), ready to write to
// the clock's data characteristic.
size_t encodeNotifyCommand(uint8_t* out, size_t cap, const char* text,
                           uint8_t priority = ScenePriority::BandHigh,
                           Animation anim = Animation_NoneAnimation,
                           Sound sound = Sound_NoneSound, Color color = Color_Black,
                           TextData_Modificator mod = TextData_Modificator_ModNone);

// Decode a Settings message (what the clock returns when reading DATA_CHAR).
// Returns true on success.
bool decodeSettings(const uint8_t* data, size_t len, Settings* out);

// Encode a Settings message (payload only, no command header) for the
// [5,0,0,0] settings-write command. Fields with has_* false are omitted.
size_t encodeSettings(uint8_t* out, size_t cap, const Settings* s);

// Encode a ForecastScene message (payload only). The caller must fill all
// required fields: timestamp (localtime-as-epoch, hour aligned), max/min,
// maxColor/minColor (RGB), values (24x Int16LE) and templateText.
size_t encodeForecastScene(uint8_t* out, size_t cap, const ForecastScene* fs);

// Encode a full forecast command: header [7, prio=16, hours=24, slot] +
// ForecastScene payload (frame layout per the HA integration / web app).
// Default slot 2: on clock fw 1.5 the factory carousel has the built-in
// digital watchface at slot 1 (protected/overwritten otherwise) and slot 0
// renders dim. Callers that manage their own slot layout can override.
size_t encodeForecastCommand(uint8_t* out, size_t cap, const ForecastScene* fs,
                             uint8_t slot = 2);

// Encode an Alarms message (payload only). Callers fill alarm[i] entries
// (enabled, days as Glance Days enum, time.hours/minutes, sound) and
// alarm_count.
size_t encodeAlarms(uint8_t* out, size_t cap, const Alarms* a);

// Encode a full alarm command: header [4, prio=0, 0, 0] + Alarms payload.
size_t encodeAlarmCommand(uint8_t* out, size_t cap, const Alarms* a);

}  // namespace Glance
