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

}  // namespace Glance
