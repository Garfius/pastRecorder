#pragma once

#include <liblame/lame.h>

#include "AudioBuffers.h"

class LameEncoder {
 public:
  ~LameEncoder() { close(); }
  bool begin(Mp3FrameRing &sink);
  bool write(const int16_t *samples, size_t count);
  bool finish();
  void close();
  bool warmUp();

 private:
  bool accept(int bytes);
  lame_t encoder_ = nullptr;
  Mp3FrameRing *sink_ = nullptr;
  uint8_t output_[8192] = {};
};
