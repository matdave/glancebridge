#include <Arduino.h>

#include "Console.h"
#include "bridge/ChronosBridge.h"
#include "glance/GlanceClient.h"
#include "glance/GlanceTimeServer.h"

static GlanceClient glance;
static GlanceTimeServer timeServer;
static ChronosBridge bridge(glance);
static Console console(glance, bridge);

void setup() {
    console.begin();
    Serial.println("[main] starting Glance Clock bridge");
    glance.begin();
    // The clock polls a Current Time Service on the central after
    // connecting; expose it (set time with 'settime' or pair the phone
    // via 'chronos on' + the Chronos app).
    timeServer.begin();
    // Start the Chronos peripheral now: pair from the Chronos app, which
    // pushes time + notifications that get relayed to the clock.
    bridge.begin();
}

void loop() {
    glance.loop();
    timeServer.loop();
    bridge.loop();
    console.poll();
    delay(20);
}
