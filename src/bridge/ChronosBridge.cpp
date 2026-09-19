#include <bridge/ChronosBridge.h>

static ChronosBridge* s_instance = nullptr;

void ChronosBridge::begin() {
    if (_started) {
        return;
    }
    s_instance = this;

    // Forward the clock's battery level to the Chronos app: on every
    // read/notification from the clock, and once when the phone connects.
    // setBattery() only flags a change; the frame goes out from _watch.loop()
    // on the main task.
    _glance.setBatteryCallback([this](int pct) {
        if (pct >= 0) {
            _watch.setBattery((uint8_t)pct);
        }
    });

    _watch.setConnectionCallback([](bool state) {
        Serial.printf("[bridge] Chronos app %s\n", state ? "connected" : "disconnected");
        // Push the clock's battery as soon as the phone connects (library
        // callbacks are plain function pointers; use the instance singleton).
        if (state && s_instance != nullptr && s_instance->_glance.lastBattery() >= 0) {
            s_instance->_watch.setBattery((uint8_t)s_instance->_glance.lastBattery());
        }
    });

    _watch.setNotificationCallback([](Notification n) {
        if (s_instance != nullptr) {
            s_instance->onNotification(n);
        }
    });

    _watch.setConfigurationCallback([](Config cfg, uint32_t a, uint32_t) {
        if (cfg == CF_TIME && a == 1) {
            // ChronosESP32 has already applied the phone time via
            // settimeofday (it inherits ESP32Time) before this fires.
            struct tm t;
            if (getLocalTime(&t)) {
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
                Serial.printf("[bridge] phone time sync: %s\n", buf);
            }
            if (s_instance->_onPhoneTime) {
                s_instance->_onPhoneTime();  // policy: accept or roll back
            }
        }
    });

    // NimBLEDevice::init is idempotent; this joins the existing NimBLE
    // device and adds our GATT server + advertising alongside the central.
    _watch.begin();
    _started = true;
    Serial.println("[bridge] Chronos peripheral started - pair from the Chronos app");
}

void ChronosBridge::loop() {
    if (_started) {
        _watch.loop();
    }
}

void ChronosBridge::onNotification(const Notification& n) {
    if (!_relay) {
        return;
    }
    if (!_glance.isConnected()) {
        Serial.println("[bridge] clock not connected, notification dropped");
        return;
    }
    String text = n.title.length() ? n.title : n.message;
    if (n.app.length() && text.length()) {
        text = n.app + ": " + text;
    }
    if (text.length() == 0) {
        return;
    }
    if (_glance.sendNotice(text.c_str())) {
        Serial.printf("[bridge] relayed: %s\n", text.c_str());
    }
}
