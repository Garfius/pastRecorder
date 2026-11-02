#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "AudioBuffers.h"

struct AudioSettings {
  float highPassHz = CFG_DEFAULT_HIGHPASS_HZ;
  float inputGain = CFG_DEFAULT_INPUT_GAIN;
  float makeupGain = CFG_DEFAULT_MAKEUP_GAIN;
  float ceiling = CFG_DEFAULT_CEILING;
  float attackMs = CFG_DEFAULT_ATTACK_MS;
  float releaseMs = CFG_DEFAULT_RELEASE_MS;
  float threshold = CFG_DEFAULT_THRESHOLD;
  float ratio = CFG_DEFAULT_RATIO;

  bool valid() const {
    return isfinite(highPassHz) && highPassHz >= 0 && highPassHz <= 2000 &&
           isfinite(inputGain) && inputGain >= 0.1f && inputGain <= 40 &&
           isfinite(makeupGain) && makeupGain >= 0.1f && makeupGain <= 40 &&
           isfinite(ceiling) && ceiling >= 0.1f && ceiling <= 0.9999f &&
           isfinite(attackMs) && attackMs >= 1 && attackMs <= 2000 &&
           isfinite(releaseMs) && releaseMs >= 1 && releaseMs <= 5000 &&
           isfinite(threshold) && threshold >= 0.0001f && threshold <= 1 &&
           isfinite(ratio) && ratio >= 1 && ratio <= 50;
  }
};

struct DeviceSettings {
  uint32_t magic = CFG_SETTINGS_MAGIC;
  uint16_t version = CFG_SETTINGS_VERSION;
  uint16_t bytes = sizeof(DeviceSettings);
  AudioSettings audio;
  uint8_t lowBatteryShutdown = 0;
  uint8_t reserved[3] = {};
  uint32_t checksum = 0;

  void seal() {
    checksum = ~crc32Update(0xFFFFFFFFu,
                           reinterpret_cast<const uint8_t *>(this),
                           offsetof(DeviceSettings, checksum));
  }
  bool valid() const {
    return magic == CFG_SETTINGS_MAGIC && version == CFG_SETTINGS_VERSION &&
           bytes == sizeof(DeviceSettings) && audio.valid() &&
           lowBatteryShutdown <= 1 &&
           checksum == ~crc32Update(0xFFFFFFFFu,
                                    reinterpret_cast<const uint8_t *>(this),
                                    offsetof(DeviceSettings, checksum));
  }
};
static_assert(sizeof(DeviceSettings) <= CFG_EEPROM_BYTES,
              "Settings must fit in EEPROM");

class DeviceSettingsStore {
 public:
  bool begin(DeviceSettings &settings, bool &usedDefaults);
  bool save(DeviceSettings &settings);
};
