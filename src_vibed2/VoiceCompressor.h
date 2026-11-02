#pragma once

#include "DeviceSettings.h"

class VoiceCompressor {
 public:
  void begin(const AudioSettings &settings) {
    settings_ = settings;
    highPass_ = 1.0f - expf(-6.28318530718f * settings.highPassHz /
                            CFG_AUDIO_SAMPLE_RATE);
    attack_ = expf(-1000.0f / (settings.attackMs * CFG_AUDIO_SAMPLE_RATE));
    release_ = expf(-1000.0f / (settings.releaseMs * CFG_AUDIO_SAMPLE_RATE));
    low_ = envelope_ = 0;
  }

  int16_t process(int32_t sample) {
    float level = static_cast<float>(sample) / 2147483648.0f;
    low_ += highPass_ * (level - low_);
    level = (level - low_) * settings_.inputGain;
    const float magnitude = fabsf(level);
    const float coefficient = magnitude > envelope_ ? attack_ : release_;
    envelope_ = coefficient * envelope_ + (1.0f - coefficient) * magnitude;
    if (envelope_ > settings_.threshold) {
      const float compressed =
          settings_.threshold +
          (envelope_ - settings_.threshold) / settings_.ratio;
      level *= compressed / envelope_;
    }
    level *= settings_.makeupGain;
    if (level > settings_.ceiling) {
      level = settings_.ceiling;
    } else if (level < -settings_.ceiling) {
      level = -settings_.ceiling;
    }
    return static_cast<int16_t>(level * 32767.0f);
  }

 private:
  AudioSettings settings_;
  float highPass_ = 0;
  float attack_ = 0;
  float release_ = 0;
  float low_ = 0;
  float envelope_ = 0;
};
