#include "DeviceSettings.h"

#include <EEPROM.h>

bool DeviceSettingsStore::begin(DeviceSettings &settings, bool &usedDefaults) {
  EEPROM.begin(CFG_EEPROM_BYTES);
  if (EEPROM.length() != CFG_EEPROM_BYTES || !EEPROM.getConstDataPtr()) {
    return false;
  }
  EEPROM.get(0, settings);
  usedDefaults = !settings.valid();
  if (usedDefaults) {
    settings = DeviceSettings{};
    return save(settings);
  }
  return true;
}

bool DeviceSettingsStore::save(DeviceSettings &settings) {
  if (!settings.audio.valid() || settings.lowBatteryShutdown > 1) {
    return false;
  }
  settings.seal();
  EEPROM.put(0, settings);
  if (!EEPROM.commit()) {
    return false;
  }
  DeviceSettings verified;
  EEPROM.get(0, verified);
  return verified.valid() && verified.checksum == settings.checksum;
}
