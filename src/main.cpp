#include <Arduino.h>

#include "Console.h"
#include "glance/GlanceClient.h"

static GlanceClient glance;
static Console console(glance);

void setup() {
    console.begin();
    Serial.println("[main] starting Glance Clock bridge");
    glance.begin();
}

void loop() {
    glance.loop();
    console.poll();
    delay(20);
}
