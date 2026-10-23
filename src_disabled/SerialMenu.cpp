#include "SerialMenu.h"

#include "RecordingStore.h"

SerialMenu::SerialMenu(Stream &serial) : serial_(serial) {}

void SerialMenu::showMain(bool fileSystemReady, const RecordingStore &store) {
  serial_.println();
  serial_.println(F("LittleFS content:"));
  if (!fileSystemReady) {
    serial_.println(F("  unavailable"));
  } else if (!store.listFiles(serial_)) {
    serial_.println(F("  (empty)"));
  }
  serial_.println();
  serial_.println(F("What to do?"));
  serial_.println(F("  1 - Record a 5 second voice message"));
  serial_.println(F("  2 - Show recording size and send by XModem"));
  serial_.print(F("> "));
}

MainAction SerialMenu::readMainAction() {
  const String choice = readLine();
  if (choice == "1") {
    return MainAction::Record;
  }
  if (choice == "2") {
    return MainAction::Send;
  }
  return MainAction::Invalid;
}

RecordingChoice SerialMenu::chooseRecording(bool wavAvailable,
                                            bool mp3Available) {
  serial_.println(F("Which recording should be sent?"));
  serial_.print(F("  1 - voice.wav"));
  if (!wavAvailable) {
    serial_.print(F(" (not available)"));
  }
  serial_.println();
  serial_.print(F("  2 - voice.mp3"));
  if (!mp3Available) {
    serial_.print(F(" (not available)"));
  }
  serial_.println();
  serial_.print(F("> "));

  const String choice = readLine();
  if (choice == "1") {
    return RecordingChoice::Wav;
  }
  if (choice == "2") {
    return RecordingChoice::Mp3;
  }
  return RecordingChoice::Invalid;
}

String SerialMenu::readLine() {
  String line;
  while (true) {
    while (serial_.available()) {
      const char character = static_cast<char>(serial_.read());
      if (character == '\r' || character == '\n') {
        if (line.length() > 0) {
          line.trim();
          return line;
        }
      } else if (character >= ' ') {
        line += character;
      }
    }
    delay(1);
  }
}