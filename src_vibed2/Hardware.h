#pragma once

#include <Arduino.h>

#include "config.h"

enum class LedState : uint8_t {
  Boot,
  Recording,
  Saving,
  Menu,
  Error,
  LowBattery,
  Off
};

class RgbLed {
 public:
  void begin();
  void set(LedState state);
  void tick();

 private:
  LedState state_ = LedState::Off;
  uint32_t since_ = 0;
};

class UserButton {
 public:
  void begin();
  void tick();
  bool takeShortPress();
  bool longPress() const { return long_; }

 private:
  bool raw_ = false;
  bool down_ = false;
  bool long_ = false;
  bool short_ = false;
  uint32_t changed_ = 0;
  uint32_t pressed_ = 0;
};

class BatteryChecker {
 public:
  void begin();
  uint16_t measure();
  bool tick();
  uint16_t millivolts() const { return millivolts_; }

 private:
  uint16_t millivolts_ = 0;
  uint32_t checked_ = 0;
};
