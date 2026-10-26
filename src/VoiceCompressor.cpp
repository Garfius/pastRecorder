#include "VoiceCompressor.h"

#include <maximilian.h>

namespace {
maxiDyn compressor;
}

void voiceCompressorBegin(uint32_t sampleRate) {
  maxiSettings::setup(sampleRate, 1, 1);
  compressor = maxiDyn();
  
  compressor.setAttack(10);
  compressor.setRelease(200);
  compressor.setThreshold(0.1);
  compressor.setRatio(8);
}

int32_t voiceCompressorProcess(int32_t sample) {
  float level = compressor.compress(static_cast<float>(sample) / 2147483648.0f);
  if (level > 0.999f) {
    level = 0.999f;
  } else if (level < -0.999f) {
    level = -0.999f;
  }
  return static_cast<int32_t>(level * 2147483648.0f);
}
