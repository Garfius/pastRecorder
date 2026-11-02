#include "VoiceCompressor.h"

#include <math.h>

#include <maximilian.h>

namespace {

constexpr float kPi = 3.14159265358979323846f;

template <typename T>
T clampValue(T value, T low, T high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

maxiDyn compressor;
float highPassCoefficient = 0.0f;
float highPassLowState = 0.0f;
VoiceCompressorParams current = {
    120.0f, 4.0f, 6.0f, 0.999f, 10.0f, 200.0f, 0.1f, 8.0f};

}  // namespace

void voiceCompressorBegin(uint32_t sampleRate,
                          const VoiceCompressorParams &params) {
  if (sampleRate == 0) {
    sampleRate = 16000;
  }

  current.highPassHz = clampValue(params.highPassHz, 20.0f, 2000.0f);
  current.inputGain = clampValue(params.inputGain, 0.1f, 40.0f);
  current.makeupGain = clampValue(params.makeupGain, 0.1f, 40.0f);
  current.ceiling = clampValue(params.ceiling, 0.1f, 0.9999f);
  current.attackMs = clampValue(params.attackMs, 1.0f, 2000.0f);
  current.releaseMs = clampValue(params.releaseMs, 1.0f, 5000.0f);
  current.threshold = clampValue(params.threshold, 0.0001f, 1.0f);
  current.ratio = clampValue(params.ratio, 1.0f, 50.0f);

  maxiSettings::setup(sampleRate, 1, 1);
  highPassCoefficient =
      1.0f - expf(-2.0f * kPi * current.highPassHz / static_cast<float>(sampleRate));
  highPassLowState = 0.0f;
  compressor = maxiDyn();
  compressor.setAttack(static_cast<int>(current.attackMs));
  compressor.setRelease(static_cast<int>(current.releaseMs));
  compressor.setThreshold(current.threshold);
  compressor.setRatio(current.ratio);
}

int32_t voiceCompressorProcess(int32_t sample) {
  float level = static_cast<float>(sample) / 2147483648.0f;
  highPassLowState += highPassCoefficient * (level - highPassLowState);
  level -= highPassLowState;
  level *= current.inputGain;
  level = compressor.compress(level);
  level *= current.makeupGain;
  level = clampValue(level, -current.ceiling, current.ceiling);
  return static_cast<int32_t>(level * 2147483648.0f);
}
