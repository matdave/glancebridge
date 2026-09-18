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

}  // namespace Glance
