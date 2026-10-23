#pragma once

#include <Arduino.h>

class RecordingStore;

enum class MainAction {
  Record,
  Send,
  Invalid
};

enum class RecordingChoice {
  Wav,
  Mp3,
  Invalid
};

class SerialMenu {
 public:
  explicit SerialMenu(Stream &serial);
  void showMain(bool fileSystemReady, const RecordingStore &store);
  MainAction readMainAction();
  RecordingChoice chooseRecording(bool wavAvailable, bool mp3Available);

 private:
  String readLine();

  Stream &serial_;
};