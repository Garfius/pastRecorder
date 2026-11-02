#pragma once

// Override these with build definitions, or edit the defaults here.
#ifndef CFG_SERIAL_PORT
#define CFG_SERIAL_PORT Serial1
#endif
#ifndef CFG_SERIAL_USB
#define CFG_SERIAL_USB 0 // Set to 0 with Serial1/Serial2: activity detection is RX-only.
#endif
#ifndef CFG_SERIAL_BAUD
#define CFG_SERIAL_BAUD 115200
#endif
#ifndef CFG_BUTTON_PIN
#define CFG_BUTTON_PIN 9
#endif
#ifndef CFG_BUTTON_ACTIVE_LOW
#define CFG_BUTTON_ACTIVE_LOW 1 // invert button logic
#endif
#ifndef CFG_BUTTON_DEBOUNCE_MS
#define CFG_BUTTON_DEBOUNCE_MS 30
#endif
#ifndef CFG_LONG_PRESS_POWER_OFF_MS
#define CFG_LONG_PRESS_POWER_OFF_MS 2500
#endif
#ifndef CFG_MENU_EXIT_REBOOT_MS
#define CFG_MENU_EXIT_REBOOT_MS 5000
#endif
#ifndef CFG_MENU_LINE_BYTES
#define CFG_MENU_LINE_BYTES 64 // serial input buffer length
#endif

#ifndef CFG_AUDIO_SAMPLE_RATE
#define CFG_AUDIO_SAMPLE_RATE 16000
#endif
#ifndef CFG_MP3_BITRATE_KBPS
#define CFG_MP3_BITRATE_KBPS 24
#endif
#ifndef CFG_MP3_LAME_QUALITY
#define CFG_MP3_LAME_QUALITY 7
#endif
#ifndef CFG_I2S_BCLK_PIN
#define CFG_I2S_BCLK_PIN 6
#endif
#ifndef CFG_I2S_LRCLK_PIN
#define CFG_I2S_LRCLK_PIN 7 // Arduino-Pico requires BCLK + 1 ¿ not used?
#endif
#ifndef CFG_I2S_DATA_PIN
#define CFG_I2S_DATA_PIN 10
#endif
#ifndef CFG_I2S_USE_RIGHT_CHANNEL
#define CFG_I2S_USE_RIGHT_CHANNEL 0
#endif
#ifndef CFG_MIC_BUFFER_COUNT
#define CFG_MIC_BUFFER_COUNT 6
#endif
#ifndef CFG_MIC_BUFFER_WORDS
#define CFG_MIC_BUFFER_WORDS 512
#endif
#ifndef CFG_PCM_RING_SAMPLES
#define CFG_PCM_RING_SAMPLES 1024 // 64 ms; four times the measured 255-sample peak.
#endif
#ifndef CFG_PCM_BLOCK_SAMPLES
#define CFG_PCM_BLOCK_SAMPLES 256
#endif
#ifndef CFG_RUNTIME_RESERVE_BYTES
#define CFG_RUNTIME_RESERVE_BYTES 24576 // AFTER live LAME, I2S and storage allocations.
#endif
#ifndef CFG_MIN_MP3_FRAMES
#define CFG_MIN_MP3_FRAMES 32
#endif
#ifndef CFG_ENCODER_TIMEOUT_MS
#define CFG_ENCODER_TIMEOUT_MS 10000
#endif
#ifndef CFG_SAMPLE_RECORD_MS
#define CFG_SAMPLE_RECORD_MS 5000
#endif

#ifndef CFG_SD_SPI
#define CFG_SD_SPI SPI
#endif
#ifndef CFG_SD_CS_PIN
#define CFG_SD_CS_PIN 5
#endif
#ifndef CFG_SD_SCK_PIN
#define CFG_SD_SCK_PIN 2
#endif
#ifndef CFG_SD_MISO_PIN
#define CFG_SD_MISO_PIN 4
#endif
#ifndef CFG_SD_MOSI_PIN
#define CFG_SD_MOSI_PIN 3
#endif
#ifndef CFG_SD_SPI_HZ
#define CFG_SD_SPI_HZ 10000000
#endif
#ifndef CFG_STORAGE_MARGIN_BYTES
#define CFG_STORAGE_MARGIN_BYTES 8192
#endif
#ifndef CFG_STORAGE_MAX_DEPTH
#define CFG_STORAGE_MAX_DEPTH 8
#endif
#ifndef CFG_STORAGE_PATH_BYTES
#define CFG_STORAGE_PATH_BYTES 192
#endif

#ifndef CFG_LED_R_PIN
#define CFG_LED_R_PIN 11
#endif
#ifndef CFG_LED_G_PIN
#define CFG_LED_G_PIN 12
#endif
#ifndef CFG_LED_B_PIN
#define CFG_LED_B_PIN 13
#endif
#ifndef CFG_LED_INVERT_LOGIC
#define CFG_LED_INVERT_LOGIC 1
#endif
#ifndef CFG_LED_PULSE_MS
#define CFG_LED_PULSE_MS 25
#endif

// Verify the VSYS/3 connection on your board before using battery protection.
#ifndef CFG_BATTERY_ADC_GPIO
#define CFG_BATTERY_ADC_GPIO 29
#endif
#ifndef CFG_BATTERY_ADC_INPUT
#define CFG_BATTERY_ADC_INPUT 3
#endif
#ifndef CFG_BATTERY_SCALE_MV
#define CFG_BATTERY_SCALE_MV 9875 //Multiplicative calibration for converting ADC readings to battery millivolts.
#endif
#ifndef CFG_BATTERY_OFFSET_MV
#define CFG_BATTERY_OFFSET_MV -20 // Fixed correction added after scaling. The default −20 subtracts 20 mV from every calculated reading.
#endif
#ifndef CFG_BATTERY_LOW_THRESHOLD_MV
#define CFG_BATTERY_LOW_THRESHOLD_MV 450
#endif
#ifndef CFG_BATTERY_RECHARGE_DELTA_MV
#define CFG_BATTERY_RECHARGE_DELTA_MV 200
#endif
#ifndef CFG_BATTERY_CHECK_INTERVAL_MS
#define CFG_BATTERY_CHECK_INTERVAL_MS 15000
#endif
#ifndef CFG_LOW_BATTERY_SIGNAL_MS
#define CFG_LOW_BATTERY_SIGNAL_MS 2000 // Duration of the final LED indication before software halt.

#endif

#ifndef CFG_EEPROM_BYTES
#define CFG_EEPROM_BYTES 512
#endif
#ifndef CFG_SETTINGS_MAGIC
#define CFG_SETTINGS_MAGIC 0x50524332u
#endif
#ifndef CFG_SETTINGS_VERSION
#define CFG_SETTINGS_VERSION 1
#endif
#ifndef CFG_DEFAULT_HIGHPASS_HZ
#define CFG_DEFAULT_HIGHPASS_HZ 120.0f
#endif
#ifndef CFG_DEFAULT_INPUT_GAIN
#define CFG_DEFAULT_INPUT_GAIN 4.0f
#endif
#ifndef CFG_DEFAULT_MAKEUP_GAIN
#define CFG_DEFAULT_MAKEUP_GAIN 6.0f
#endif
#ifndef CFG_DEFAULT_CEILING
#define CFG_DEFAULT_CEILING 0.999f
#endif
#ifndef CFG_DEFAULT_ATTACK_MS
#define CFG_DEFAULT_ATTACK_MS 10.0f
#endif
#ifndef CFG_DEFAULT_RELEASE_MS
#define CFG_DEFAULT_RELEASE_MS 200.0f
#endif
#ifndef CFG_DEFAULT_THRESHOLD
#define CFG_DEFAULT_THRESHOLD 0.1f
#endif
#ifndef CFG_DEFAULT_RATIO
#define CFG_DEFAULT_RATIO 8.0f
#endif
