#include <Arduino.h>

// Select Serial for USB CDC or Serial1 for TTL UART on the board's default pins.
#define CFG_SERIAL_PORT Serial1

#define CFG_SERIAL_BAUD 115200

namespace {
auto &serialPortUsed = CFG_SERIAL_PORT;
}

void setup() {
  serialPortUsed.begin(CFG_SERIAL_BAUD);
}

void loop() {
  const int bytesAvailable = serialPortUsed.available();
  if (bytesAvailable > 0) {
    for (int index = 0; index < bytesAvailable; ++index) {
      serialPortUsed.read();
    }
    serialPortUsed.println("i see you");
  }
}
