#pragma once

#include <Arduino.h>
#include <I2S.h>

#include "RecordingStore.h"

class VoiceRecorder {
 public:
  explicit VoiceRecorder(Print &output);
  bool record(RecordingStore &store);

 private:
  Print &output_;
  I2S microphone_;
};
/**
 * At 16KHz 16 bit using
*/
class Mp3Transcoder {
 public:
  explicit Mp3Transcoder(Print &output);
  bool encode(RecordingStore &store);

 private:
  Print &output_;
};