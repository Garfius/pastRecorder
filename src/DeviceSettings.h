#pragma once

#include <stdint.h>

#include "config.h"

struct DeviceSettings {
  uint32_t magic;
  uint16_t version;
  uint8_t lowBatteryShutdown;
  uint8_t reserved0;

  uint32_t nextFileId;

  float highPassHz;
  float inputGain;
  float makeupGain;
  float ceiling;

  float attackMs;
  float releaseMs;
  float threshold;
  float ratio;

  uint16_t lowBatteryThresholdMv;
  uint16_t rechargeThresholdMv;
  uint32_t batteryCheckIntervalMs;

  uint8_t reserved[24];
};

static_assert(sizeof(DeviceSettings) <= CFG_EEPROM_BYTES,
              "DeviceSettings must fit into EEPROM");

class DeviceSettingsStore {
 public:
  bool begin();
  bool load(DeviceSettings *settings);
  bool save(const DeviceSettings &settings);

  static DeviceSettings defaults();

 private:
  bool validate(const DeviceSettings &settings) const;

  bool ready_ = false;
};
