#include <Arduino.h>

#include "Console.h"
#include "bridge/ChronosBridge.h"
#include "glance/GlanceClient.h"
#include "glance/GlanceTimeServer.h"
#include "net/Forecast.h"
#include "net/NetTime.h"
#include "net/WebPortal.h"

static GlanceClient glance;
static GlanceTimeServer timeServer;
static NetTime net;
static ChronosBridge bridge(glance);
static Forecast forecast(glance);
static WebPortal portal(glance, bridge, net, forecast);
static Console console(glance, bridge, net, forecast, portal);

void setup() {
    console.begin();
    Serial.println("[main] starting Glance Clock bridge");
    net.begin();  // WiFi + NTP if credentials are stored ('wifi <ssid> <pass>')
    glance.begin();
    // The clock polls a Current Time Service on the central after
    // connecting; expose it. Time sources, in priority order: NTP
    // (once 'wifi' is configured), the Chronos phone app, 'settime'.
    timeServer.begin();
    // Phone time pushes are applied by the Chronos library before we see
    // the event (its ESP32Time::setTime now sets tm_isdst=-1 via our local
    // patch, so the components convert correctly). NTP still wins whenever
    // it is valid - the phone push is only needed when there is no WiFi.
    bridge.onPhoneTime([]() {
        if (net.timeValid() && net.restoreTime()) {
            Serial.println("[bridge] phone time rejected - NTP is authoritative");
        } else {
            Serial.println("[bridge] phone time accepted (no NTP)");
        }
        glance.refreshClockTime();
    });
    // Start the Chronos peripheral: pair from the Chronos app, which
    // pushes time + notifications that get relayed to the clock.
    bridge.begin();
    // 24h forecast ring (Open-Meteo) if a location is stored ('geo').
    forecast.begin();
    // Web portal: loads the stored mDNS name ('mdns <name>'); the HTTP
    // server itself starts lazily once WiFi is up.
    portal.begin();
}

static bool timeWasValid = false;

void loop() {
    net.loop();
    // When the system time first becomes valid (NTP sync), nudge the clock
    // to re-poll the Current Time Service immediately instead of waiting
    // for its own schedule.
    bool timeValid = net.timeValid();
    if (timeValid && !timeWasValid) {
        glance.refreshClockTime();
    }
    timeWasValid = timeValid;
    glance.loop();
    timeServer.loop();
    bridge.loop();
    forecast.loop();
    portal.handle();  // web PIN entry (headless pairing)
    console.poll();
    delay(20);
}

