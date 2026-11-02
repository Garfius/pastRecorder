#include "VoiceCompressor.h"

#include <math.h>
#include <maximilian.h>

namespace {
constexpr float kHighPassHz = 120.0f;  // removes rumble, wind and handling noise
constexpr float kInputGain = 4.0f;     // +12 dB before compression
constexpr float kMakeupGain = 6.0f;    // +15.5 dB after compression
constexpr float kCeiling = 0.999f;      // hard limiter, as a fraction of full scale

maxiDyn compressor;
float highPassCoefficient = 0.0f;
float highPassLowState = 0.0f;
}

void voiceCompressorBegin(uint32_t sampleRate) {
  maxiSettings::setup(sampleRate, 1, 1);
  highPassCoefficient = 1.0f - expf(-2.0f * static_cast<float>(M_PI) * kHighPassHz /
                                    static_cast<float>(sampleRate));
  highPassLowState = 0.0f;
  compressor = maxiDyn();

  compressor.setAttack(10);
  compressor.setRelease(200);
  compressor.setThreshold(0.1);
  compressor.setRatio(8);
}

int32_t voiceCompressorProcess(int32_t sample) {
  float level = static_cast<float>(sample) / 2147483648.0f;
  highPassLowState += highPassCoefficient * (level - highPassLowState);
  level -= highPassLowState;
  level *= kInputGain;
  level = compressor.compress(level);
  level *= kMakeupGain;
  if (level > kCeiling) {
    level = kCeiling;
  } else if (level < -kCeiling) {
    level = -kCeiling;
  }
  return static_cast<int32_t>(level * 2147483648.0f);
}
