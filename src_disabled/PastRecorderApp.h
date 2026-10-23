#pragma once

#include <Arduino.h>

#include "AudioServices.h"
#include "RecordingStore.h"
#include "SerialMenu.h"
#include "XModemSender.h"

class PastRecorderApp {
 public:
  explicit PastRecorderApp(HardwareSerial &serial);
  void begin();
  void runOnce();

 private:
  void sendRecording();

  HardwareSerial &serial_;
  RecordingStore store_;
  SerialMenu menu_;
  VoiceRecorder recorder_;
  Mp3Transcoder transcoder_;
  XModemSender sender_;
};