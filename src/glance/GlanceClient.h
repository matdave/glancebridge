#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <functional>

// BLE central for the Glance Clock.
//
// Flow:
//   begin()                -> init NimBLE + security (bonding, MITM, keyboard IO)
//   scan(ms)               -> collect devices advertising the Glance service
//   pair()                 -> connect + SMP pairing (clock shows a PIN on its
//                             LEDs; promptOnPasskey prints it request to Serial)
//   sendCommand()          -> write framed commands to the data characteristic
//   loop()                 -> automatic reconnection using the bonded address
//
// Bonds are stored in NVS by NimBLE, so after the first pairing a reboot
// reconnects silently (no PIN prompt).

class GlanceClient : public NimBLEClientCallbacks, public NimBLEScanCallbacks {
public:
    // Called when the clock wants the PIN it displays on its LEDs.
    // Return the 6-digit passkey. (Default impl reads from Serial.)
    using PinProvider = std::function<uint32_t()>;

    void begin();
    void loop();

    // Scan for Glance clocks. Returns number of devices found.
    int scan(uint32_t durationMs = 5000);
    void printScanResults() const;

    // Connect + pair with a device found by scan() (or the stored address).
    // Press the clock's pairing button first. Blocks while pairing.
    bool pair();

    bool isConnected() const { return _connected && _client != nullptr && _client->isConnected(); }
    bool hasStoredAddress() const { return _storedAddr.length() > 0; }
    String storedAddress() const { return _storedAddr; }

    // Write a framed command (header + protobuf payload) to the clock.
    bool sendCommand(const uint8_t* data, size_t len);
    bool sendCommand(uint8_t type, uint8_t prio, uint8_t a = 0, uint8_t b = 0,
                     const uint8_t* payload = nullptr, size_t payloadLen = 0);

    // Convenience: notify with text (see GlanceMessages.h for option defaults).
    bool sendNotice(const char* text);

    // Request + read the clock's current settings (needs a refresh first;
    // this is what the official web app does). Diagnostics.
    bool readSettings();

    // Dump the clock's full GATT table (names + values) over serial.
    void dumpGattTable();

    // (Un)subscribe to the undocumented 8e400001 notify channel.
    bool subscribePush(bool on);

    // Last settings payload read from the clock (hex, for debugging).
    String lastSettingsHex() const { return _settingsHex; }

    void forget();   // clear stored address + our side of nothing (NVS addr only)
    void setPinProvider(PinProvider provider) { _pinProvider = provider; }

    // NimBLEClientCallbacks
    void onConnect(NimBLEClient* pClient) override;
    void onDisconnect(NimBLEClient* pClient, int reason) override;
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

    // NimBLEScanCallbacks
    void onResult(const NimBLEAdvertisedDevice* device) override;

private:
    bool connect(const NimBLEAdvertisedDevice* device);
    bool discoverDataCharacteristic();
    bool secureAndDiscover();
    void handleNotify(uint8_t* data, size_t len);

    NimBLEClient* _client = nullptr;
    NimBLERemoteCharacteristic* _dataChar = nullptr;
    bool _connected = false;
    bool _authenticated = false;
    bool _scanning = false;

    // last notification from the clock's data characteristic
    uint8_t _notifBuf[256] = {0};
    size_t _notifLen = 0;
    volatile bool _notifReady = false;

    PinProvider _pinProvider;
    std::vector<const NimBLEAdvertisedDevice*> _scanResults;

    Preferences _prefs;
    String _storedAddr;
    uint8_t _storedType = 0;
    uint32_t _nextReconnectMs = 0;
    String _settingsHex;
};
