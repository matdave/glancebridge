#include <unity.h>

#include <glance/GlanceMessages.h>

// Reference frame from the Glance protocol docs:
// gatttool write 023000002203120141 == Notify(priority=48) + Notice{'A'}
// Pulse animation + Radar (General_alert_1) sound + Lime color are defaults.
static const uint8_t REFERENCE_FRAME[] = {0x02, 0x30, 0x00, 0x00, 0x22, 0x03, 0x12, 0x01, 0x41};

void test_make_header_frame() {    uint8_t h[4];
    size_t n = Glance::makeHeader(h, Glance::Cmd::Notify, Glance::ScenePriority::BandHigh, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_HEX8(0x02, h[0]);
    TEST_ASSERT_EQUAL_HEX8(0x30, h[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, h[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, h[3]);
}

void test_reference_notify_frame() {
    uint8_t buf[64];
    size_t len = Glance::encodeNotifyCommand(buf, sizeof(buf), "A");
    TEST_ASSERT_EQUAL_UINT(sizeof(REFERENCE_FRAME), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(REFERENCE_FRAME, buf, len);
}

void test_notice_custom_color_and_modificator() {
    uint8_t buf[64];
    // Notice{type=Pulse(1), sound=Radar(5), color=Red(5),
    //        text=[TextData{modificators=Rapid(2), text="Hi"}]}
    // -> 08 01 10 05 18 05 22 06 08 02 12 02 48 69
    const uint8_t expected[] = {0x08, 0x01, 0x10, 0x05, 0x18, 0x05,
                                0x22, 0x06, 0x08, 0x02, 0x12, 0x02, 0x48, 0x69};
    size_t len = Glance::encodeNotice(buf, sizeof(buf), "Hi", Animation_Pulse, Sound_Radar,
                                      Color_Red, TextData_Modificator_Rapid);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, len);
}

void test_settings_decode() {
    // Settings with ALL required fields present (nanopb rejects messages
    // missing any required field):
    // nightModeEnabled=true(2), dateFormat=DateDisabled(5),
    // pointsAlwaysEnabled=false(9), displayBrightness=5(10),
    // timeModeEnable=true(11), timeFormat12=true(12)
    const uint8_t data[] = {0x10, 0x01, 0x28, 0x00, 0x48, 0x00,
                            0x50, 0x05, 0x58, 0x01, 0x60, 0x01};
    Settings settings;
    TEST_ASSERT_TRUE(Glance::decodeSettings(data, sizeof(data), &settings));
    TEST_ASSERT_TRUE(settings.nightModeEnabled);
    TEST_ASSERT_EQUAL_INT32(5, settings.displayBrightness);
    TEST_ASSERT_TRUE(settings.timeFormat12);
}

void test_settings_decode_partial() {
    // The clock's live settings may omit fields; all Settings fields are
    // optional now, so a partial message must decode.
    const uint8_t data[] = {0x10, 0x01, 0x50, 0x05};  // nightModeEnabled=true, brightness=5
    Settings settings;
    TEST_ASSERT_TRUE(Glance::decodeSettings(data, sizeof(data), &settings));
    TEST_ASSERT_TRUE(settings.has_nightModeEnabled);
    TEST_ASSERT_TRUE(settings.nightModeEnabled);
    TEST_ASSERT_EQUAL_INT32(5, settings.displayBrightness);
    TEST_ASSERT_FALSE(settings.has_timeFormat12);
}

void test_make_command_rejects_small_buffer() {
    uint8_t small[4];
    uint8_t payload[] = {0x41};
    size_t len = Glance::makeCommand(small, sizeof(small), Glance::Cmd::Notify,
                                     Glance::ScenePriority::BandHigh, 0, 0, payload, 1);
    TEST_ASSERT_EQUAL_UINT(0, len);
}

void test_notify_text_truncates_to_max_size() {
    char big[200];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    uint8_t buf[256];
    size_t len = Glance::encodeNotifyCommand(buf, sizeof(buf), big);
    // must not overflow: text is capped at TextData.text max_size (64)
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(len < sizeof(buf));
}

void test_settings_encode_decode_roundtrip() {
    Settings s = Settings_init_default;
    s.has_nightModeEnabled = true;
    s.nightModeEnabled = true;
    s.has_displayBrightness = true;
    s.displayBrightness = 200;
    s.has_timeModeEnable = true;
    s.timeModeEnable = true;
    s.has_timeFormat12 = true;
    s.timeFormat12 = false;
    s.has_dateFormat = true;
    s.dateFormat = Settings_DateFormat_DateDisabled;
    uint8_t buf[80];
    size_t len = Glance::encodeSettings(buf, sizeof(buf), &s);
    TEST_ASSERT_TRUE(len > 0);
    Settings out;
    TEST_ASSERT_TRUE(Glance::decodeSettings(buf, len, &out));
    TEST_ASSERT_TRUE(out.has_nightModeEnabled);
    TEST_ASSERT_TRUE(out.nightModeEnabled);
    TEST_ASSERT_EQUAL_INT32(200, out.displayBrightness);
    TEST_ASSERT_TRUE(out.has_timeModeEnable);
    TEST_ASSERT_TRUE(out.timeModeEnable);
    TEST_ASSERT_FALSE(out.timeFormat12);
    TEST_ASSERT_EQUAL_INT32(Settings_DateFormat_DateDisabled, out.dateFormat);
}

void test_alarm_command_frame() {
    // Header must be [4, 0, 0, 0] (Alarm command) + Alarms protobuf with
    // two alarms: 07:30 Monday and 22:00 all days.
    Alarms alarms = Alarms_init_default;
    alarms.alarm_count = 2;

    alarms.alarm[0].has_enabled = true;
    alarms.alarm[0].enabled = true;
    alarms.alarm[0].has_days = true;
    alarms.alarm[0].days = Days_Monday;
    alarms.alarm[0].time.hours = 7;
    alarms.alarm[0].time.minutes = 30;
    alarms.alarm[0].sound = Sound_NoneSound;

    alarms.alarm[1].has_enabled = true;
    alarms.alarm[1].enabled = true;
    alarms.alarm[1].has_days = true;
    alarms.alarm[1].days = Days_All;
    alarms.alarm[1].time.hours = 22;
    alarms.alarm[1].time.minutes = 0;
    alarms.alarm[1].sound = Sound_NoneSound;

    uint8_t buf[128];
    size_t len = Glance::encodeAlarmCommand(buf, sizeof(buf), &alarms);
    TEST_ASSERT_TRUE(len > 4);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[0]);  // Alarm command
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[1]);  // prio 0
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[3]);

    Alarms out;
    pb_istream_t stream = pb_istream_from_buffer(buf + 4, len - 4);
    TEST_ASSERT_TRUE(pb_decode(&stream, Alarms_fields, &out));
    TEST_ASSERT_EQUAL_UINT(2, out.alarm_count);
    TEST_ASSERT_TRUE(out.alarm[0].enabled);
    TEST_ASSERT_EQUAL_INT32(Days_Monday, out.alarm[0].days);
    TEST_ASSERT_EQUAL_INT32(7, out.alarm[0].time.hours);
    TEST_ASSERT_EQUAL_INT32(30, out.alarm[0].time.minutes);
    TEST_ASSERT_EQUAL_INT32(Days_All, out.alarm[1].days);
    TEST_ASSERT_EQUAL_INT32(22, out.alarm[1].time.hours);
}

void test_forecast_command_frame() {
    // Header must be [7, 16, 24, slot] (SaveForecastScene, medium prio,
    // 24 hours) per the HA integration / web app, followed by the protobuf.
    // Default slot is 2 (factory watchface lives at slot 1 on clock fw 1.5).
    ForecastScene fs = ForecastScene_init_default;
    fs.timestamp = 1780000000LL;  // local wall time encoded as epoch
    fs.maxColor = 0xFF0000;
    fs.minColor = 0x0000FF;
    fs.max = 30;
    fs.min = 10;
    for (int i = 0; i < 24; i++) {
        int16_t v = (int16_t)(10 + i);  // rising temps
        fs.values.bytes[i * 2] = (uint8_t)(v & 0xFF);
        fs.values.bytes[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    fs.values.size = 48;
    static const uint8_t tmpl[] = {0xC2, 0x8F, 0x08, 0xC2, 0xB0, 0x43};
    memcpy(fs.templateText.bytes, tmpl, sizeof(tmpl));
    fs.templateText.size = sizeof(tmpl);

    uint8_t buf[128];
    size_t len = Glance::encodeForecastCommand(buf, sizeof(buf), &fs);
    TEST_ASSERT_TRUE(len > 4);
    TEST_ASSERT_EQUAL_HEX8(0x07, buf[0]);   // SaveForecastScene
    TEST_ASSERT_EQUAL_HEX8(0x10, buf[1]);   // priority 16
    TEST_ASSERT_EQUAL_HEX8(0x18, buf[2]);   // 24 hours
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[3]);   // slot 2 (factory watchface is slot 1)
    // Payload decodes back into the same scene.
    ForecastScene out;
    pb_istream_t stream = pb_istream_from_buffer(buf + 4, len - 4);
    TEST_ASSERT_TRUE(pb_decode(&stream, ForecastScene_fields, &out));
    TEST_ASSERT_EQUAL_INT64(1780000000LL, out.timestamp);
    TEST_ASSERT_EQUAL_INT32(30, out.max);
    TEST_ASSERT_EQUAL_INT32(10, out.min);
    TEST_ASSERT_EQUAL_UINT(48, out.values.size);
    int16_t first = (int16_t)(out.values.bytes[0] | (out.values.bytes[1] << 8));
    int16_t last =
        (int16_t)(out.values.bytes[46] | (out.values.bytes[47] << 8));
    TEST_ASSERT_EQUAL_INT16(10, first);
    TEST_ASSERT_EQUAL_INT16(33, last);
    TEST_ASSERT_EQUAL_UINT(sizeof(tmpl), out.templateText.size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tmpl, out.templateText.bytes, sizeof(tmpl));
}


