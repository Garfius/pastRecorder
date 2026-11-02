#include "DeviceSettings.h"

#include <EEPROM.h>

namespace {

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

}  // namespace

bool DeviceSettingsStore::begin() {
  EEPROM.begin(CFG_EEPROM_BYTES);
  ready_ = true;
  return ready_;
}

DeviceSettings DeviceSettingsStore::defaults() {
  DeviceSettings settings = {};
  settings.magic = CFG_SETTINGS_MAGIC;
  settings.version = CFG_SETTINGS_VERSION;
  settings.lowBatteryShutdown = 0;
  settings.nextFileId = 1;

  settings.highPassHz = CFG_DEFAULT_HIGHPASS_HZ;
  settings.inputGain = CFG_DEFAULT_INPUT_GAIN;
  settings.makeupGain = CFG_DEFAULT_MAKEUP_GAIN;
  settings.ceiling = CFG_DEFAULT_CEILING;

  settings.attackMs = CFG_DEFAULT_ATTACK_MS;
  settings.releaseMs = CFG_DEFAULT_RELEASE_MS;
  settings.threshold = CFG_DEFAULT_THRESHOLD;
  settings.ratio = CFG_DEFAULT_RATIO;

  settings.lowBatteryThresholdMv = CFG_BATTERY_LOW_THRESHOLD_MV;
  settings.rechargeThresholdMv = CFG_BATTERY_RECHARGE_DELTA_MV;
  settings.batteryCheckIntervalMs = CFG_BATTERY_CHECK_INTERVAL_MS;
  return settings;
}

bool DeviceSettingsStore::load(DeviceSettings *settings) {
  if (!ready_ || !settings) {
    return false;
  }

  DeviceSettings candidate = {};
  EEPROM.get(0, candidate);
  if (!validate(candidate)) {
    candidate = defaults();
    if (!save(candidate)) {
      return false;
    }
  }

  *settings = candidate;
  return true;
}

bool DeviceSettingsStore::save(const DeviceSettings &settings) {
  if (!ready_) {
    return false;
  }

  DeviceSettings persisted = settings;
  persisted.magic = CFG_SETTINGS_MAGIC;
  persisted.version = CFG_SETTINGS_VERSION;

  persisted.nextFileId = persisted.nextFileId == 0 ? 1 : persisted.nextFileId;

  persisted.highPassHz = clampValue(persisted.highPassHz, 20.0f, 2000.0f);
  persisted.inputGain = clampValue(persisted.inputGain, 0.1f, 40.0f);
  persisted.makeupGain = clampValue(persisted.makeupGain, 0.1f, 40.0f);
  persisted.ceiling = clampValue(persisted.ceiling, 0.1f, 0.9999f);
  persisted.attackMs = clampValue(persisted.attackMs, 1.0f, 2000.0f);
  persisted.releaseMs = clampValue(persisted.releaseMs, 1.0f, 5000.0f);
  persisted.threshold = clampValue(persisted.threshold, 0.0001f, 1.0f);
  persisted.ratio = clampValue(persisted.ratio, 1.0f, 50.0f);

  if (persisted.lowBatteryThresholdMv < 2500 ||
      persisted.lowBatteryThresholdMv > 4300) {
    persisted.lowBatteryThresholdMv = CFG_BATTERY_LOW_THRESHOLD_MV;
  }
  if (persisted.rechargeThresholdMv > 1000) {
    persisted.rechargeThresholdMv = CFG_BATTERY_RECHARGE_DELTA_MV;
  }
  if (persisted.batteryCheckIntervalMs < 1000 ||
      persisted.batteryCheckIntervalMs > 3600000UL) {
    persisted.batteryCheckIntervalMs = CFG_BATTERY_CHECK_INTERVAL_MS;
  }

  EEPROM.put(0, persisted);
  return EEPROM.commit();
}

bool DeviceSettingsStore::validate(const DeviceSettings &settings) const {
  if (settings.magic != CFG_SETTINGS_MAGIC ||
      settings.version != CFG_SETTINGS_VERSION) {
    return false;
  }

  if (settings.nextFileId == 0) {
    return false;
  }
  if (settings.highPassHz < 20.0f || settings.highPassHz > 2000.0f) {
    return false;
  }
  if (settings.inputGain < 0.1f || settings.inputGain > 40.0f) {
    return false;
  }
  if (settings.makeupGain < 0.1f || settings.makeupGain > 40.0f) {
    return false;
  }
  if (settings.ceiling <= 0.0f || settings.ceiling > 1.0f) {
    return false;
  }
  if (settings.attackMs < 1.0f || settings.attackMs > 2000.0f) {
    return false;
  }
  if (settings.releaseMs < 1.0f || settings.releaseMs > 5000.0f) {
    return false;
  }
  if (settings.threshold <= 0.0f || settings.threshold > 1.0f) {
    return false;
  }
  if (settings.ratio < 1.0f || settings.ratio > 50.0f) {
    return false;
  }
  if (settings.lowBatteryThresholdMv < 2500 ||
      settings.lowBatteryThresholdMv > 4300) {
    return false;
  }
  if (settings.rechargeThresholdMv > 1000) {
    return false;
  }
  if (settings.batteryCheckIntervalMs < 1000 ||
      settings.batteryCheckIntervalMs > 3600000UL) {
    return false;
  }

  return true;
}
