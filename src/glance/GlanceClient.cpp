#include "GlanceClient.h"

#include <glance/GlanceCommands.h>
#include <glance/GlanceMessages.h>

static const char* TAG = "GlanceClient";
static const char* NVS_NS = "glance";
static const char* NVS_ADDR = "clockAddr";
static const char* NVS_ADDR_TYPE = "clockAddrType";

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
            for (NimBLERemoteDescriptor* dsc : chr->getDescriptors()) {
                Serial.printf("[%s]     desc %s handle=0x%04x\n", TAG,
                              dsc->getUUID().toString().c_str(), dsc->getHandle());
            }
        }
    }
    Serial.printf("[%s] ---------------------------\n", TAG);
}

bool GlanceClient::discoverDataCharacteristic() {
    // NimBLE 2.x discovers lazily; run the full discovery explicitly so the
    // dump shows every attribute and getService() uses the complete cache.
    if (_client->discoverAttributes()) {
        dumpGattTable();
    } else {
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
    Serial.printf("[%s] data characteristic canNotify=%d canIndicate=%d\n", TAG,
                  _dataChar->canNotify() ? 1 : 0, _dataChar->canIndicate() ? 1 : 0);

    // Identify the other Glance characteristics via their 0x2901 descriptions
    for (NimBLERemoteCharacteristic* chr : svc->getCharacteristics()) {
        if (chr->getUUID() == NimBLEUUID(Glance::DATA_CHAR_UUID)) {
            continue;
        }
        NimBLERemoteDescriptor* d = chr->getDescriptor(NimBLEUUID((uint16_t)0x2901));
        String name = d ? d->readValue().c_str() : "";
        Serial.printf("[%s] char %s '%s'\n", TAG, chr->getUUID().toString().c_str(),
                      name.c_str());
        if (chr->canRead()) {
            NimBLEAttValue v = chr->readValue();
            Serial.printf("[%s]   value (%u bytes): %s\n", TAG, (unsigned)v.length(),
                          toHex((const uint8_t*)v.data(), v.length()).c_str());
        }
    }
    readSettings();
    return true;
}

void GlanceClient::handleNotify(uint8_t* data, size_t len) {
    if (len > sizeof(_notifBuf)) {
        len = sizeof(_notifBuf);
    }
    memcpy(_notifBuf, data, len);
    _notifLen = len;
    _notifReady = true;
}

bool GlanceClient::readSettings() {
    if (_dataChar == nullptr) {
        return false;
    }
    // The data characteristic is read+write without notify: write a Settings
    // "get" (5,0,0,0) and read the response back from the same characteristic.
    if (!sendCommand(Glance::Cmd::Settings, 0, 0, 0)) {
        return false;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        delay(150);
        NimBLEAttValue value = _dataChar->readValue();
        if (value.length() == 0) {
            continue;
        }
        _settingsHex = toHex((const uint8_t*)value.data(), value.length());
        Serial.printf("[%s] settings response (%u bytes): %s\n", TAG, (unsigned)value.length(),
                      _settingsHex.c_str());
        Settings settings;
        if (Glance::decodeSettings((const uint8_t*)value.data(), value.length(), &settings)) {
            Serial.printf("[%s] settings: nightMode=%d brightness=%d 12h=%d\n", TAG,
                          settings.nightModeEnabled ? 1 : 0, settings.displayBrightness,
                          settings.timeFormat12 ? 1 : 0);
            return true;
        }
        Serial.printf("[%s] settings decode failed\n", TAG);
        return false;
    }
    Serial.printf("[%s] settings response empty\n", TAG);
    return false;
}

// ----------------------------------------------------------------- commands

bool GlanceClient::sendCommand(const uint8_t* data, size_t len) {
    if (!isConnected() || _dataChar == nullptr) {
        Serial.printf("[%s] not connected\n", TAG);
        return false;
    }
    bool ok = _dataChar->writeValue(data, len, true);
    if (!ok) {
        Serial.printf("[%s] write failed\n", TAG);
    }
    return ok;
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

// ------------------------------------------------------------------ forget

void GlanceClient::forget() {
    _prefs.remove(NVS_ADDR);
    _prefs.remove(NVS_ADDR_TYPE);
    _storedAddr = "";
    Serial.printf("[%s] stored address cleared\n", TAG);
}
