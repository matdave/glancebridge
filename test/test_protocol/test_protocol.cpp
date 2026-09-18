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

