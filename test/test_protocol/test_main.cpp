#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_make_header_frame(void);
void test_reference_notify_frame(void);
void test_notice_custom_color_and_modificator(void);
void test_settings_decode(void);
void test_settings_decode_partial(void);
void test_make_command_rejects_small_buffer(void);
void test_notify_text_truncates_to_max_size(void);
void test_settings_encode_decode_roundtrip(void);

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_make_header_frame);
    RUN_TEST(test_reference_notify_frame);
    RUN_TEST(test_notice_custom_color_and_modificator);
    RUN_TEST(test_settings_decode);
    RUN_TEST(test_settings_decode_partial);
    RUN_TEST(test_make_command_rejects_small_buffer);
    RUN_TEST(test_notify_text_truncates_to_max_size);
    RUN_TEST(test_settings_encode_decode_roundtrip);
    return UNITY_END();
}
