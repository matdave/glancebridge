#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <glance/GlanceMessages.h>

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

    // Called whenever the clock's battery level is read or notified
    // (percent 0-100). Runs on the NimBLE host task for notifications -
    // keep it short, no blocking BLE calls.
    using BatteryCallback = std::function<void(int)>;

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

    // Write raw bytes to the Scene characteristic (5075ffac) - for testing
    // commands the firmware rejects on the Data characteristic (0x81).
    bool sendSceneCommand(const uint8_t* data, size_t len);

    // Install the clock's own digital watchface as a carousel scene
    // (CustomScene cmd 0, display mode 8 = built-in watchface). Gives the
    // carousel a bright face to return to after scene pushes like the
    // forecast ring (an empty slot renders dim).
    bool installWatchfaceScene(uint8_t slot = 0);
    bool watchfaceInstalled() const { return _watchfaceInstalled; }

    // Convenience: notify with text (see GlanceMessages.h for option defaults).
    bool sendNotice(const char* text);

    // Read the clock's settings: plain read of the data characteristic; if
    // empty, request a refresh with single-byte cmd 35 (4-byte frame form is
    // rejected, vendor ATT 0x81) and poll. Best effort - the clock often
    // publishes nothing. Diagnostics only.
    bool readSettings();

    // Last successfully decoded settings from the clock (defaults until the
    // first successful read). Base for read-modify-write settings changes.
    Settings lastSettings() const { return _lastSettings; }

    // Write a complete Settings message ([5,0,0,0] + protobuf, the path the
    // HA integration uses for brightness/mode changes). Replaces ALL clock
    // settings - callers must fill every field.
    bool writeSettings(const Settings* s);

        // Read the clock's battery level (standard 0x180F/0x2A19). Returns
    // percent 0-100, or -1 if unavailable. Cached for status.
    int readBattery();
    int lastBattery() const { return _battPercent; }

    // Subscribe to the clock's battery notifications and call the callback
    // on every value (also fires once right after subscribing).
    void setBatteryCallback(BatteryCallback cb) { _battCallback = std::move(cb); }

    // Reconnect the clock so it re-polls the Current Time Service with the
    // fresh system time (call when the time source changes: first NTP sync,
    // phone time sync, settime). No-op when not connected - the regular
    // reconnect covers that case.
    void refreshClockTime();

    // Dump the clock's full GATT table (names + values) over serial.
    void dumpGattTable();

    // Clean disconnect (diagnostic: does the clock keep its bond when the
    // disconnect is deliberate vs a supervision timeout?). Reconnects on
    // the next loop() pass.
    void disconnect() {
        if (_client != nullptr && _client->isConnected()) {
            Serial.printf("[GlanceClient] clean disconnect requested\n");
            _client->disconnect();
            _nextReconnectMs = millis() + 5000;
        }
    }

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
    void updateBattery(uint8_t pct);
    void subscribeBattery();

    NimBLEClient* _client = nullptr;
    NimBLERemoteCharacteristic* _dataChar = nullptr;
    NimBLERemoteCharacteristic* _battChar = nullptr;
    NimBLERemoteCharacteristic* _sceneChar = nullptr;
    bool _pairingInProgress = false;  // true during deliberate pair() (PIN ok)
    bool _watchfaceInstalled = false;  // slot 0 installed this boot?
    int _battPercent = -1;
    bool _connected = false;
    bool _authenticated = false;
    bool _scanning = false;

    // last notification from the clock's data characteristic
    uint8_t _notifBuf[256] = {0};
    size_t _notifLen = 0;
    volatile bool _notifReady = false;

    PinProvider _pinProvider;
    BatteryCallback _battCallback;
    std::vector<const NimBLEAdvertisedDevice*> _scanResults;

    Preferences _prefs;
    String _storedAddr;
    uint8_t _storedType = 0;
    uint32_t _nextReconnectMs = 0;
    uint32_t _lastNudgeMs = 0;  // last refreshClockTime() (debounce)
    String _settingsHex;
    Settings _lastSettings = Settings_init_default;
};
