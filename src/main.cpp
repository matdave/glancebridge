#include <Arduino.h>

#include "Console.h"
#include "glance/GlanceClient.h"
#include "glance/GlanceTimeServer.h"

static GlanceClient glance;
static GlanceTimeServer timeServer;
static Console console(glance);

void setup() {
    console.begin();
    Serial.println("[main] starting Glance Clock bridge");
    glance.begin();
    // The clock polls a Current Time Service on the central after
    // connecting; expose it (set time manually with 'settime' until the
    // Chronos phone sync is wired up).
    timeServer.begin();
}

void loop() {
    glance.loop();
    timeServer.loop();
    console.poll();
    delay(20);
}
