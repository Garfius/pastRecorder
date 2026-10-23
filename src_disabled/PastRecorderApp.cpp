#include "PastRecorderApp.h"

#include "AudioConfig.h"

PastRecorderApp::PastRecorderApp(HardwareSerial &serial)
    : serial_(serial), menu_(serial), recorder_(serial), transcoder_(serial),
      sender_(serial) {}

void PastRecorderApp::begin() {
  serial_.begin(115200);
  const uint32_t serialWaitStart = millis();
  while (!serial_ && millis() - serialWaitStart < 2000) {
    delay(10);
  }

  serial_.println();
  serial_.println(F("Past Recorder"));
  serial_.println(F("Hello! Ready to record a voice message."));

  const bool fileSystemReady = store_.begin();
  serial_.println(fileSystemReady ? F("LittleFS mounted.")
                                  : F("LittleFS mount failed."));

  if (!sender_.begin()) {
    serial_.println(F("XModem initialization failed."));
  }
}

void PastRecorderApp::runOnce() {
  menu_.showMain(store_.isReady(), store_);
  const MainAction action = menu_.readMainAction();
  if (action == MainAction::Record) {
    if (recorder_.record(store_)) {
      transcoder_.encode(store_);
    }
  } else if (action == MainAction::Send) {
    sendRecording();
  } else {
    serial_.println(F("Choose 1 or 2."));
  }
}

void PastRecorderApp::sendRecording() {
  if (!store_.isReady()) {
    serial_.println(F("LittleFS is not available."));
    return;
  }
  if (!sender_.isReady()) {
    serial_.println(F("XModem is not available."));
    return;
  }

  const bool wavAvailable = store_.exists(kRecordingPath);
  const bool mp3Available = store_.exists(kMp3RecordingPath);
  if (!wavAvailable && !mp3Available) {
    serial_.println(F("No voice.wav or voice.mp3 recording is available."));
    return;
  }

  const RecordingChoice choice =
      menu_.chooseRecording(wavAvailable, mp3Available);
  if (choice == RecordingChoice::Invalid) {
    serial_.println(F("Choose 1 or 2."));
    return;
  }

  sender_.sendFile(store_, choice == RecordingChoice::Wav ? kRecordingPath
                                                         : kMp3RecordingPath);
}