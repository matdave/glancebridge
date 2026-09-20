#include <glance/GlanceMessages.h>

#include <string.h>

namespace Glance {

static size_t appendTextData(TextData* td, const char* text, TextData_Modificator mod) {
    if (mod != TextData_Modificator_ModNone) {
        td->has_modificators = true;
        td->modificators = mod;
    }
    size_t len = strlen(text);
    if (len > sizeof(td->text.bytes)) {
        len = sizeof(td->text.bytes);
    }
    memcpy(td->text.bytes, text, len);
    td->text.size = (pb_size_t)len;
    return len;
}

size_t encodeNotice(uint8_t* out, size_t cap, const char* text, Animation anim, Sound sound,
                    Color color, TextData_Modificator mod) {
    if (out == nullptr || cap == 0 || text == nullptr) {
        return 0;
    }

    Notice notice = Notice_init_default;
    // Passing the zero value of an optional field means "omit it and let the
    // clock apply its default" (the documented reference frame relies on this).
    if (anim != Animation_NoneAnimation) {
        notice.has_type = true;
        notice.type = anim;
    }
    if (sound != Sound_NoneSound) {
        notice.has_sound = true;
        notice.sound = sound;
    }
    if (color != Color_Black) {
        notice.has_color = true;
        notice.color = color;
    }

    notice.text_count = 1;
    appendTextData(&notice.text[0], text, mod);

    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, Notice_fields, &notice)) {
        return 0;
    }
    return stream.bytes_written;
}

size_t encodeNotifyCommand(uint8_t* out, size_t cap, const char* text, uint8_t priority,
                           Animation anim, Sound sound, Color color, TextData_Modificator mod) {
    uint8_t payload[128];
    size_t payloadLen = encodeNotice(payload, sizeof(payload), text, anim, sound, color, mod);
    if (payloadLen == 0) {
        return 0;
    }
    return makeCommand(out, cap, Cmd::Notify, priority, 0, 0, payload, payloadLen);
}

bool decodeSettings(const uint8_t* data, size_t len, Settings* out) {
    if (data == nullptr || out == nullptr) {
        return false;
    }
    *out = Settings_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    return pb_decode(&stream, Settings_fields, out);
}

size_t encodeSettings(uint8_t* out, size_t cap, const Settings* s) {
    if (out == nullptr || cap == 0 || s == nullptr) {
        return 0;
    }
    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, Settings_fields, s)) {
        return 0;
    }
    return stream.bytes_written;
}

size_t encodeForecastScene(uint8_t* out, size_t cap, const ForecastScene* fs) {
    if (out == nullptr || cap == 0 || fs == nullptr) {
        return 0;
    }
    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, ForecastScene_fields, fs)) {
        return 0;
    }
    return stream.bytes_written;
}

size_t encodeForecastCommand(uint8_t* out, size_t cap, const ForecastScene* fs, uint8_t slot) {
    uint8_t payload[96];
    size_t payloadLen = encodeForecastScene(payload, sizeof(payload), fs);
    if (payloadLen == 0) {
        return 0;
    }
    // [7, 16, 24, slot]: SaveForecastScene, medium priority, 24 hours.
    return makeCommand(out, cap, Cmd::SaveForecastScene, ScenePriority::BandMedium, 24, slot,
                       payload, payloadLen);
}
size_t encodeAlarms(uint8_t* out, size_t cap, const Alarms* a) {
    if (out == nullptr || cap == 0 || a == nullptr) {
        return 0;
    }
    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, Alarms_fields, a)) {
        return 0;
    }
    return stream.bytes_written;
}

size_t encodeAlarmCommand(uint8_t* out, size_t cap, const Alarms* a) {
    uint8_t payload[192];
    size_t payloadLen = encodeAlarms(payload, sizeof(payload), a);
    if (payloadLen == 0) {
        return 0;
    }
    return makeCommand(out, cap, Cmd::Alarm, 0, 0, 0, payload, payloadLen);
}

}  // namespace Glance
