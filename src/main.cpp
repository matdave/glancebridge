#include <Arduino.h>

#include "Console.h"
#include "bridge/ChronosBridge.h"
#include "glance/GlanceClient.h"
#include "glance/GlanceTimeServer.h"
#include "net/NetTime.h"

static GlanceClient glance;
static GlanceTimeServer timeServer;
static NetTime net;
static ChronosBridge bridge(glance);
static Console console(glance, bridge, net);

void setup() {
    console.begin();
    Serial.println("[main] starting Glance Clock bridge");
    net.begin();  // WiFi + NTP if credentials are stored ('wifi <ssid> <pass>')
    glance.begin();
    // The clock polls a Current Time Service on the central after
    // connecting; expose it. Time sources, in priority order: NTP
    // (once 'wifi' is configured), the Chronos phone app, 'settime'.
    timeServer.begin();
    // Start the Chronos peripheral: pair from the Chronos app, which
    // pushes time + notifications that get relayed to the clock.
    bridge.begin();
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
    console.poll();
    delay(20);
}

