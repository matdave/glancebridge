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

    // ALL callbacks below run on the NimBLE HOST task: never do blocking
    // work here (GATT writes overflow the host stack - canary crash; lwIP
    // calls assert on the TCPIP lock). They only set pending flags; the
    // actual work happens in loop() on the main task.
    _watch.setConfigurationCallback([](Config cfg, uint32_t a, uint32_t) {
        if (s_instance == nullptr) {
            return;
        }
        if (cfg == CF_TIME && a == 1) {
            // ChronosESP32 already applied the phone time via settimeofday
            // (it inherits ESP32Time) before this fires.
            Serial.println("[bridge] phone time config received");
            s_instance->_phoneTimePending = true;
        } else if (cfg == CF_ALARM) {
            Serial.println("[bridge] alarm change received");
            s_instance->_alarmPushPending = true;
        }
    });

    // Incoming call: the Chronos app forwards the ringer state with the
    // caller's name. There is no CallScene payload spec in the proto, so
    // the call is shown as a Notice with the phone icon (byte 129, the
    // raw icon code; 0x81 is a valid TextData byte). The notice dismisses
    // itself - nothing to do on call end.
    _watch.setRingerCallback([](String name, bool start) {
        if (s_instance == nullptr) {
            return;
        }
        if (start) {
            s_instance->queueNotice(String((char)129) + " " + name);
            Serial.printf("[bridge] incoming call: %s\n", name.c_str());
        } else {
            Serial.printf("[bridge] call ended: %s\n", name.c_str());
        }
    });

    // NimBLEDevice::init is idempotent; this joins the existing NimBLE
    // device and adds our GATT server + advertising alongside the central.
    _watch.begin();
    _started = true;
    Serial.println("[bridge] Chronos peripheral started - pair from the Chronos app");
}

void ChronosBridge::loop() {
    if (!_started) {
        return;
    }
    // Drain host-task pending flags here, on the main task, where BLE
    // writes and lwIP calls are safe.
    bool phoneTime = _phoneTimePending;
    _phoneTimePending = false;
    bool alarmPush = _alarmPushPending;
    _alarmPushPending = false;
    bool notice = _noticePending;
    String text(_noticeBuf);
    _noticePending = false;

    _watch.loop();

    if (phoneTime && _onPhoneTime) {
        _onPhoneTime();  // policy: accept or roll back (+ CTS nudge)
    }
    if (alarmPush) {
        pushAlarms();
    }
    if (notice) {
        relayNotice(text);
    }
}

void ChronosBridge::queueNotice(const String& text) {
    if (text.length() == 0) {
        return;
    }
    strncpy(_noticeBuf, text.c_str(), sizeof(_noticeBuf) - 1);
    _noticeBuf[sizeof(_noticeBuf) - 1] = '\0';
    _noticePending = true;
}

void ChronosBridge::relayNotice(const String& text) {
    if (!_relay) {
        return;
    }
    if (!_glance.isConnected()) {
        Serial.println("[bridge] clock not connected, notification dropped");
        return;
    }
    if (_glance.sendNotice(text.c_str())) {
        Serial.printf("[bridge] relayed: %s\n", text.c_str());
    }
}

void ChronosBridge::onNotification(const Notification& n) {
    // Host task: only build the text and hand it to the main loop.
    if (!_relay) {
        return;
    }
    // The library's splitTitle puts the REAL content in `message` and
    // falls back to the app name for `title` when the text has no early
    // colon ("Message: Message" relic). Prefer message; prefix the app
    // name unless it is already the whole text.
    String text;
    if (n.message.length()) {
        text = n.message;
    } else {
        text = n.title;
    }
    if (n.app.length() && text.length() && n.app != text) {
        text = n.app + ": " + text;
    }
    queueNotice(text);
}

// ------------------------------------------------------------ alarm sync

// Chronos alarm repeat bitfield -> Glance Days enum. Chronos: bit0=Mon ..
// bit5=Sat, bit6=Sun, 0x7F = every day, 0x80 = once. Glance: None=0,
// Monday=1..Sunday=7, All=8. Multiple weekdays are approximated as All.
static int32_t mapRepeatToDays(uint8_t repeat) {
    if (repeat == 0 || repeat == 0x80) {
        return Days_None;
    }
    if (repeat == 0x7F) {
        return Days_All;
    }
    int bits = __builtin_popcount(repeat);
    if (bits > 1) {
        return Days_All;
    }
    for (int bit = 0; bit < 7; bit++) {
        if (repeat & (1 << bit)) {
            // bit0=Mon..bit5=Sat -> Days 1..6; bit6=Sun -> Days 7
            return (bit == 6) ? Days_Sunday : (int32_t)(Days_Monday + bit);
        }
    }
    return Days_None;
}

void ChronosBridge::pushAlarms() {
    if (!_glance.isConnected()) {
        Serial.println("[bridge] clock not connected, alarms not pushed");
        return;
    }
    Alarms alarms = Alarms_init_default;
    for (int i = 0; i < CS_ALARM_SIZE; i++) {
        Alarm a = _watch.getAlarm(i);
        if (!a.enabled || alarms.alarm_count >= 8) {
            continue;
        }
        AlarmData& d = alarms.alarm[alarms.alarm_count];
        d.has_enabled = true;
        d.enabled = true;
        d.has_days = true;
        d.days = mapRepeatToDays(a.repeat);
        d.time.hours = a.hour;
        d.time.minutes = a.minute;
        // Audible: Sound_NoneSound would make the alarm display silently.
        // Waves is the proto's "Alarm" sound (the classic alarm tone).
        d.sound = Sound_Waves;
        alarms.alarm_count++;
    }
    uint8_t frame[192];
    size_t len = Glance::encodeAlarmCommand(frame, sizeof(frame), &alarms);
    if (len == 0) {
        Serial.println("[bridge] alarm encode failed");
        return;
    }
    if (_glance.sendCommand(frame, len)) {
        Serial.printf("[bridge] pushed %u alarm(s) to the clock\n",
                      (unsigned)alarms.alarm_count);
    }
}

void ChronosBridge::printAlarms() {
    int enabled = 0;
    for (int i = 0; i < CS_ALARM_SIZE; i++) {
        Alarm a = _watch.getAlarm(i);
        if (!a.enabled) {
            continue;
        }
        Serial.printf("[bridge] alarm %d: %02u:%02u repeat=0x%02X\n", i, a.hour,
                      a.minute, a.repeat);
        enabled++;
    }
    if (enabled == 0) {
        Serial.println("[bridge] no alarms set in the Chronos app");
    }
}
