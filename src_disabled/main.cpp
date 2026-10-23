#include <Arduino.h>

#include "PastRecorderApp.h"
#include "pico/multicore.h"

PastRecorderApp app(Serial1);

void setup() {
  app.begin();
}

void loop() {
  app.runOnce();
}