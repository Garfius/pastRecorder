#pragma once

#include <stdint.h>

struct VoiceCompressorParams {
  float highPassHz;
  float inputGain;
  float makeupGain;
  float ceiling;

  float attackMs;
  float releaseMs;
  float threshold;
  float ratio;
};

// Resets the compressor state for a mono stream at sampleRate.
void voiceCompressorBegin(uint32_t sampleRate,
                          const VoiceCompressorParams &params);

// High-passes, gains, compresses and limits one full-scale signed 32-bit sample.
int32_t voiceCompressorProcess(int32_t sample);
