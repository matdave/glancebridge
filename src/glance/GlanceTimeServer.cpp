#include <glance/GlanceTimeServer.h>

void GlanceTimeServer::refresh() {
    if (_currentTime == nullptr) {
        return;
    }
    struct tm t;
    if (!getLocalTime(&t, 50)) {
        return;  // system time not set yet (epoch)
    }
    uint8_t buf[10];
    uint16_t year = (uint16_t)(t.tm_year + 1900);
    buf[0] = (uint8_t)(year & 0xFF);
    buf[1] = (uint8_t)((year >> 8) & 0xFF);
    buf[2] = (uint8_t)(t.tm_mon + 1);
    buf[3] = (uint8_t)t.tm_mday;
    buf[4] = (uint8_t)t.tm_hour;
    buf[5] = (uint8_t)t.tm_min;
    buf[6] = (uint8_t)t.tm_sec;
    buf[7] = (uint8_t)((t.tm_wday == 0) ? 7 : t.tm_wday);  // 1=Mon .. 7=Sun
    buf[8] = 0;                                            // fractions of 1/256 s
    buf[9] = 0;                                            // adjust reason flags
    _currentTime->setValue(buf, sizeof(buf));
}

void GlanceTimeServer::begin() {
    if (_started) {
        return;
    }
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server == nullptr) {
        server = NimBLEDevice::createServer();
    }
    NimBLEService* svc = server->getServiceByUUID(NimBLEUUID((uint16_t)0x1805));
    if (svc == nullptr) {
        svc = server->createService(NimBLEUUID((uint16_t)0x1805));
    }
    _currentTime = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A2B));
    if (_currentTime == nullptr) {
        _currentTime = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A2B),
                                                 NIMBLE_PROPERTY::READ);
        refresh();
        svc->start();
    }
    _started = true;
}

void GlanceTimeServer::loop() {
    if (!_started) {
        return;
    }
    if ((int32_t)(millis() - _nextRefreshMs) < 0) {
        return;
    }
    _nextRefreshMs = millis() + 1000;
    refresh();
}
