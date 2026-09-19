#include "GlanceClient.h"

#include <glance/GlanceCommands.h>
#include <glance/GlanceMessages.h>

static const char* TAG = "GlanceClient";
static const char* NVS_NS = "glance";
static const char* NVS_ADDR = "clockAddr";
static const char* NVS_ADDR_TYPE = "clockAddrType";

// Undocumented bidirectional channel (read+write+notify) seen in the GATT
// dump; suspected response/push channel for commands sent to 'Data'.
static const char* UNKNOWN_SVC_UUID = "8e400001-f315-4f60-9fb8-838830daea50";

// ---------------------------------------------------------------- utilities

static String toHex(const uint8_t* data, size_t len) {
    String out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", data[i]);
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------- lifecycle

void GlanceClient::begin() {
    NimBLEDevice::init("GlanceBridge");

    // Bonding + MITM (passkey). LE Secure Connections is negotiated down if
    // the clock (2017-era nRF52) does not support it.
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    _prefs.begin(NVS_NS, false);
    if (_prefs.isKey(NVS_ADDR)) {
        _storedAddr = _prefs.getString(NVS_ADDR, "");
        _storedType = _prefs.getUChar(NVS_ADDR_TYPE, 0);
        Serial.printf("[%s] stored clock address: %s\n", TAG, _storedAddr.c_str());
    }

    _client = NimBLEDevice::createClient();
    if (_client != nullptr) {
        _client->setClientCallbacks(this, false);
        _client->setConnectTimeout(5000);
    }
}

void GlanceClient::loop() {
    if (isConnected() || _storedAddr.length() == 0 || _scanning) {
        return;
    }
    if ((int32_t)(millis() - _nextReconnectMs) < 0) {
        return;
    }
    _nextReconnectMs = millis() + 10000;

    Serial.printf("[%s] reconnecting to %s...\n", TAG, _storedAddr.c_str());
    NimBLEAddress addr(_storedAddr.c_str(), _storedType);
    if (_client->connect(addr)) {
        if (!secureAndDiscover()) {
            _client->disconnect();
        }
    }
}

// ------------------------------------------------------------------- scan

void GlanceClient::onResult(const NimBLEAdvertisedDevice* device) {
    String name = device->getName().c_str();
    bool serviceMatch = device->isAdvertisingService(NimBLEUUID(Glance::SERVICE_UUID));
    bool nameMatch = name.indexOf("Glance") >= 0;
    if (serviceMatch || nameMatch) {
        Serial.printf("[%s] found clock: %s (%s)%s\n", TAG, name.c_str(),
                      device->getAddress().toString().c_str(), serviceMatch ? "" : " [by name]");
        _scanResults.push_back(device);
    }
}

int GlanceClient::scan(uint32_t durationMs) {
    _scanResults.clear();
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(this, false);
    pScan->setActiveScan(true);
    pScan->clearResults();
    _scanning = true;
    NimBLEScanResults results = pScan->getResults(durationMs);
    _scanning = false;
    return (int)_scanResults.size();
}

void GlanceClient::printScanResults() const {
    Serial.printf("[%s] %d candidate(s):\n", TAG, (int)_scanResults.size());
    for (auto* dev : _scanResults) {
        Serial.printf("  %s  %s\n", dev->getAddress().toString().c_str(),
                      dev->getName().c_str());
    }
}

// ------------------------------------------------------------------ pairing

void GlanceClient::onPassKeyEntry(NimBLEConnInfo& connInfo) {
    Serial.printf("[%s] passkey requested (conn handle %u)\n", TAG, connInfo.getConnHandle());
    uint32_t passkey = _pinProvider ? _pinProvider() : 0;
    NimBLEDevice::injectPassKey(connInfo, passkey);
}

void GlanceClient::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    // Logging only: secureConnection() in the main task tracks the result.
    if (connInfo.isEncrypted()) {
        Serial.printf("[%s] authentication complete, bonded=%d\n", TAG,
                      connInfo.isBonded() ? 1 : 0);
    } else {
        Serial.printf("[%s] authentication FAILED\n", TAG);
    }
}

void GlanceClient::onConnect(NimBLEClient* pClient) {
    _connected = true;
    Serial.printf("[%s] connected, MTU=%u\n", TAG, pClient->getMTU());
}

void GlanceClient::onDisconnect(NimBLEClient* pClient, int reason) {
    _connected = false;
    _authenticated = false;
    _dataChar = nullptr;
    _battChar = nullptr;
    Serial.printf("[%s] disconnected, reason=0x%04x\n", TAG, reason);
}

bool GlanceClient::pair() {
    const NimBLEAdvertisedDevice* target = nullptr;

    if (!_scanResults.empty()) {
        target = _scanResults.front();
    } else if (hasStoredAddress()) {
        NimBLEAddress addr(_storedAddr.c_str(), _storedType);
        NimBLEScan* pScan = NimBLEDevice::getScan();
        pScan->setScanCallbacks(this, false);
        pScan->setActiveScan(true);
        pScan->clearResults();
        _scanning = true;
        NimBLEScanResults results = pScan->getResults(5000);
        _scanning = false;
        for (int i = 0; i < results.getCount(); i++) {
            if (results.getDevice(i)->getAddress() == addr) {
                target = results.getDevice(i);
                break;
            }
        }
        if (target == nullptr) {
            Serial.printf("[%s] stored clock not found in scan\n", TAG);
            return false;
        }
    } else {
        Serial.printf("[%s] nothing to pair with; run 'scan' first\n", TAG);
        return false;
    }

    if (_client == nullptr) {
        _client = NimBLEDevice::createClient();
        _client->setClientCallbacks(this, false);
    }

    Serial.printf("[%s] connecting to %s ...\n", TAG, target->getAddress().toString().c_str());
    if (!connect(target)) {
        Serial.printf("[%s] connect failed\n", TAG);
        return false;
    }

    if (_dataChar == nullptr && !discoverDataCharacteristic()) {
        return false;
    }

    _storedAddr = target->getAddress().toString().c_str();
    _storedType = target->getAddress().getType();
    _prefs.putString(NVS_ADDR, _storedAddr);
    _prefs.putUChar(NVS_ADDR_TYPE, _storedType);
    Serial.printf("[%s] paired successfully with %s\n", TAG, _storedAddr.c_str());
    return true;
}

bool GlanceClient::connect(const NimBLEAdvertisedDevice* device) {
    if (!_client->connect(device)) {
        return false;
    }
    return secureAndDiscover();
}

// Start SMP pairing / re-encryption and discover the data characteristic.
// MUST run in the main task: calling blocking BLE functions from inside
// NimBLE callbacks (host task) deadlocks the stack.
bool GlanceClient::secureAndDiscover() {
    if (!_client->secureConnection()) {
        Serial.printf("[%s] secureConnection failed\n", TAG);
        return false;
    }
    _authenticated = true;
    return discoverDataCharacteristic();
}

void GlanceClient::dumpGattTable() {
    Serial.printf("[%s] ---- remote GATT table ----\n", TAG);
    for (NimBLERemoteService* svc : _client->getServices()) {
        Serial.printf("[%s] svc %s\n", TAG, svc->getUUID().toString().c_str());
        for (NimBLERemoteCharacteristic* chr : svc->getCharacteristics()) {
            Serial.printf("[%s]   chr %s handle=0x%04x props:%s%s%s%s%s\n", TAG,
                          chr->getUUID().toString().c_str(), chr->getHandle(),
                          chr->canRead() ? " read" : "", chr->canWrite() ? " write" : "",
                          chr->canWriteNoResponse() ? " writeNR" : "",
                          chr->canNotify() ? " notify" : "", chr->canIndicate() ? " indicate" : "");
            NimBLERemoteDescriptor* d = chr->getDescriptor(NimBLEUUID((uint16_t)0x2901));
            if (d != nullptr) {
                NimBLEAttValue name = d->readValue();
                Serial.printf("[%s]     name: '%s'\n", TAG, name.c_str());
            }
            if (chr->canRead()) {
                NimBLEAttValue v = chr->readValue();
                Serial.printf("[%s]     value (%u bytes): %s\n", TAG, (unsigned)v.length(),
                              toHex((const uint8_t*)v.data(), v.length()).c_str());
            }
        }
    }
    Serial.printf("[%s] ---------------------------\n", TAG);
}

bool GlanceClient::subscribePush(bool on) {
    NimBLERemoteService* unkSvc = _client->getService(NimBLEUUID(UNKNOWN_SVC_UUID));
    if (unkSvc == nullptr) {
        Serial.printf("[%s] 8e400001 service not found\n", TAG);
        return false;
    }
    NimBLERemoteCharacteristic* unkChr = unkSvc->getCharacteristic(NimBLEUUID(UNKNOWN_SVC_UUID));
    if (unkChr == nullptr || !unkChr->canNotify()) {
        Serial.printf("[%s] 8e400001 characteristic not notifiable\n", TAG);
        return false;
    }
    bool ok = on ? unkChr->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* data,
                                                  size_t len, bool) { handleNotify(data, len); })
                 : unkChr->unsubscribe();
    Serial.printf("[%s] 8e400001 %s %s\n", TAG, on ? "subscribe" : "unsubscribe",
                  ok ? "ok" : "FAILED");
    return ok;
}

bool GlanceClient::discoverDataCharacteristic() {
    // NimBLE 2.x discovers lazily; run the full discovery explicitly so
    // getService() uses the complete cache (dump on demand via 'gatt').
    if (!_client->discoverAttributes()) {
        Serial.printf("[%s] full GATT discovery failed\n", TAG);
    }
    NimBLERemoteService* svc = _client->getService(NimBLEUUID(Glance::SERVICE_UUID));
    if (svc == nullptr) {
        Serial.printf("[%s] Glance service not found\n", TAG);
        return false;
    }
    _dataChar = svc->getCharacteristic(NimBLEUUID(Glance::DATA_CHAR_UUID));
    if (_dataChar == nullptr) {
        Serial.printf("[%s] data characteristic not found\n", TAG);
        return false;
    }
    // Settings read is manual ('settings' console command): a plain read of
    // the data characteristic. It never triggers the cloud-update animation.
    // Also cache the standard battery characteristic (0x180F/0x2A19) for the
    // 'batt' command; optional - failure here doesn't affect commands.
    NimBLERemoteService* battSvc = _client->getService(NimBLEUUID((uint16_t)0x180F));
    _battChar = battSvc ? battSvc->getCharacteristic(NimBLEUUID((uint16_t)0x2A19)) : nullptr;
    return true;
}

void GlanceClient::handleNotify(uint8_t* data, size_t len) {
    if (len > sizeof(_notifBuf)) {
        len = sizeof(_notifBuf);
    }
    memcpy(_notifBuf, data, len);
    _notifLen = len;
    _notifReady = true;
    Serial.printf("[%s] notification (%u bytes): %s\n", TAG, (unsigned)len,
                  toHex(data, len).c_str());
}

bool GlanceClient::readSettings() {
    if (_dataChar == nullptr) {
        return false;
    }
    // Read + strip + decode whatever the data characteristic holds. Envelopes
    // seen in the wild: "Data\0" + protobuf, [5,0,0,0] + protobuf, or a
    // single 0x05 byte before the protobuf. (A raw protobuf can never start
    // with 0x05: that would be field 0.)
    auto readPublished = [this]() -> bool {
        NimBLEAttValue v = _dataChar->readValue();
        if (v.length() < 5) {
            return false;
        }
        const uint8_t* p = (const uint8_t*)v.data();
        size_t len = v.length();
        if (memcmp(p, "Data", 4) == 0 && p[4] == 0x00) {
            p += 5;
            len -= 5;
        } else if (p[0] == Glance::Cmd::Settings && p[1] == 0x00 && p[2] == 0x00 &&
                   p[3] == 0x00) {
            p += 4;
            len -= 4;
        } else if (p[0] == Glance::Cmd::Settings) {
            p += 1;
            len -= 1;
        }
        if (len == 0) {
            return false;
        }
        _settingsHex = toHex(p, len);
        Serial.printf("[%s] settings read (%u bytes): %s\n", TAG, (unsigned)len,
                      _settingsHex.c_str());
        Settings settings;
        if (Glance::decodeSettings(p, len, &settings)) {
            _lastSettings = settings;
            Serial.printf("[%s] settings: nightMode=%d brightness=%d 12h=%d\n", TAG,
                          settings.nightModeEnabled ? 1 : 0, settings.displayBrightness,
                          settings.timeFormat12 ? 1 : 0);
            return true;
        }
        Serial.printf("[%s] settings decode failed\n", TAG);
        return false;
    };

    // HA integration flow: plain read first; if nothing is published, send
    // command 35 (UpdateAndRefresh) as a SINGLE byte - the 4-byte frame is
    // rejected with vendor ATT error 0x81, and sendCommand() falls back to a
    // no-response write which the clock accepts - then poll the read again.
    if (readPublished()) {
        return true;
    }
    Serial.printf("[%s] nothing published; requesting refresh (single-byte cmd 35)\n", TAG);
    const uint8_t refresh[] = {Glance::Cmd::UpdateAndRefresh};
    if (!sendCommand(refresh, sizeof(refresh))) {
        Serial.printf("[%s] refresh request failed\n", TAG);
        return false;
    }
    for (int poll = 0; poll < 10; poll++) {
        delay(300);
        if (readPublished()) {
            return true;
        }
    }
    Serial.printf("[%s] settings read: nothing published in data characteristic\n", TAG);
    return false;
}

bool GlanceClient::writeSettings(const Settings* s) {
    uint8_t payload[80];
    size_t payloadLen = Glance::encodeSettings(payload, sizeof(payload), s);
    if (payloadLen == 0) {
        Serial.printf("[%s] settings encode failed\n", TAG);
        return false;
    }
    // HA-style settings write: [5,0,0,0] + complete Settings protobuf. This
    // replaces all settings on the clock - callers must send every field.
    return sendCommand(Glance::Cmd::Settings, 0, 0, 0, payload, payloadLen);
}

// ------------------------------------------------------------------ battery

int GlanceClient::readBattery() {
    if (!isConnected()) {
        Serial.printf("[%s] not connected\n", TAG);
        return -1;
    }
    if (_battChar == nullptr) {
        NimBLERemoteService* svc = _client->getService(NimBLEUUID((uint16_t)0x180F));
        _battChar = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A19)) : nullptr;
        if (_battChar == nullptr || !_battChar->canRead()) {
            Serial.printf("[%s] battery characteristic not available\n", TAG);
            _battChar = nullptr;
            return -1;
        }
    }
    NimBLEAttValue v = _battChar->readValue();
    if (v.length() < 1) {
        Serial.printf("[%s] battery read: empty value\n", TAG);
        return -1;
    }
    uint8_t pct = v.data()[0];
    if (pct > 100) {
        pct = 100;  // some firmwares report raw 0-255
    }
    _battPercent = pct;
    Serial.printf("[%s] battery: %u%%\n", TAG, (unsigned)pct);
    return pct;
}

// ----------------------------------------------------------------- commands

bool GlanceClient::sendCommand(const uint8_t* data, size_t len) {
    if (!isConnected() || _dataChar == nullptr) {
        Serial.printf("[%s] not connected\n", TAG);
        return false;
    }
    // Visible start log: a write with response blocks until the clock ACKs
    // or the host's 30s GATT timeout fires (which drops the connection).
    Serial.printf("[%s] sending cmd 0x%02x (%u bytes)\n", TAG, data[0], (unsigned)len);
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (_dataChar->writeValue(data, len, true)) {
            return true;
        }
        Serial.printf("[%s] write failed (attempt %d/3)\n", TAG, attempt);
        // The clock rejects some commands (e.g. command 35) as with-response
        // writes with vendor ATT error 0x81, but accepts them as writes
        // without response - the HA integration falls back exactly this way
        // for every command. A no-response write gives no ACK either way, so
        // treat acceptance as unconfirmed (the caller's follow-up read/write
        // is the real test).
        if (_dataChar->writeValue(data, len, false)) {
            Serial.printf("[%s] no-response write accepted\n", TAG);
            return true;
        }
        delay(100);
    }
    return false;
}

bool GlanceClient::sendCommand(uint8_t type, uint8_t prio, uint8_t a, uint8_t b,
                               const uint8_t* payload, size_t payloadLen) {
    uint8_t frame[256];
    size_t len = Glance::makeCommand(frame, sizeof(frame), type, prio, a, b, payload, payloadLen);
    if (len == 0) {
        return false;
    }
    return sendCommand(frame, len);
}

bool GlanceClient::sendNotice(const char* text) {
    uint8_t frame[160];
    size_t len = Glance::encodeNotifyCommand(frame, sizeof(frame), text);
    if (len == 0) {
        return false;
    }
    return sendCommand(frame, len);
}

// --------------------------------------------------------------------- time

void GlanceClient::refreshClockTime() {
    if (!isConnected()) {
        return;
    }
    Serial.printf("[%s] time changed - reconnecting clock so it re-polls the time service\n",
                  TAG);
    _client->disconnect();
    _nextReconnectMs = 0;  // reconnect on the next loop() pass
}

// ------------------------------------------------------------------ forget

void GlanceClient::forget() {
    _prefs.remove(NVS_ADDR);
    _prefs.remove(NVS_ADDR_TYPE);
    _storedAddr = "";
    Serial.printf("[%s] stored address cleared\n", TAG);
}
