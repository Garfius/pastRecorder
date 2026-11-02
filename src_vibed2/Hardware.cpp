#include "Hardware.h"

#include <hardware/adc.h>

void RgbLed::begin() {
  pinMode(CFG_LED_R_PIN, OUTPUT);
  pinMode(CFG_LED_G_PIN, OUTPUT);
  pinMode(CFG_LED_B_PIN, OUTPUT);
  set(LedState::Boot);
}

void RgbLed::set(LedState state) {
  state_ = state;
  since_ = millis();
  tick();
}

void RgbLed::tick() {
  uint32_t period = 1000;
  unsigned color = 0;
  switch (state_) {
    case LedState::Boot: color = 1; break;
    case LedState::Recording: color = 2; period = 2000; break;
    case LedState::Saving: color = 3; period = 200; break;
    case LedState::Menu: color = 1; period = 1000; break;
    case LedState::Error: color = 4; period = 250; break;
    case LedState::LowBattery: color = 6; period = 250; break;
    case LedState::Off: break;
  }
  const bool on = (millis() - since_) % period < CFG_LED_PULSE_MS;
  const auto write = [on, color](uint8_t pin, unsigned mask) {
    const bool lit = on && (color & mask);
    digitalWrite(pin, (lit != static_cast<bool>(CFG_LED_INVERT_LOGIC)) ?
                         HIGH : LOW);
  };
  write(CFG_LED_R_PIN, 4);
  write(CFG_LED_G_PIN, 2);
  write(CFG_LED_B_PIN, 1);
}

void UserButton::begin() {
  pinMode(CFG_BUTTON_PIN, CFG_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  changed_ = millis();
}

void UserButton::tick() {
  const uint32_t now = millis();
  const bool raw = digitalRead(CFG_BUTTON_PIN) ==
                   (CFG_BUTTON_ACTIVE_LOW ? LOW : HIGH);
  if (raw != raw_) {
    raw_ = raw;
    changed_ = now;
  }
  if (raw_ != down_ && now - changed_ >= CFG_BUTTON_DEBOUNCE_MS) {
    down_ = raw_;
    if (down_) {
      pressed_ = now;
    } else if (!long_) {
      short_ = true;
    }
  }
  if (down_ && now - pressed_ >= CFG_LONG_PRESS_POWER_OFF_MS) {
    long_ = true;
  }
}

bool UserButton::takeShortPress() {
  const bool pressed = short_;
  short_ = false;
  return pressed;
}

void BatteryChecker::begin() {
  static_assert(CFG_BATTERY_ADC_GPIO == 26 + CFG_BATTERY_ADC_INPUT,
                "Battery GPIO must match ADC channel");
  adc_init();
  adc_gpio_init(CFG_BATTERY_ADC_GPIO);
  measure();
}

uint16_t BatteryChecker::measure() {
  // Cooperative Arduino port of TuriSc/RP2040-Battery-Check's VSYS calibration.
  adc_select_input(CFG_BATTERY_ADC_INPUT);
  adc_read();
  uint32_t sum = 0;
  for (unsigned i = 0; i < 16; ++i) {
    sum += adc_read();
  }
  const int32_t mv = static_cast<int32_t>(
      sum * CFG_BATTERY_SCALE_MV / (16u * 4096u)) + CFG_BATTERY_OFFSET_MV;
  millivolts_ = mv > 0 ? static_cast<uint16_t>(mv) : 0;
  checked_ = millis();
  return millivolts_;
}

bool BatteryChecker::tick() {
  if (millis() - checked_ < CFG_BATTERY_CHECK_INTERVAL_MS) {
    return false;
  }
  measure();
  return true;
}
