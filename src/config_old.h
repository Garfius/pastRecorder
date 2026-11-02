#pragma once

// ------------------------------
// Serial and user interaction
// ------------------------------
#define CFG_SERIAL_PORT Serial1
#define CFG_SERIAL_BAUD 115200

// BOOTSEL long press powers off without setting low-battery shutdown state.
#ifndef CFG_LONG_PRESS_POWER_OFF_MS
#define CFG_LONG_PRESS_POWER_OFF_MS 2500
#endif

// Polling BOOTSEL is expensive (it idles the other core), so keep this modest.
#ifndef CFG_BOOTSEL_POLL_MS
#define CFG_BOOTSEL_POLL_MS 25
#endif

#ifndef CFG_MENU_EXIT_REBOOT_MS
#define CFG_MENU_EXIT_REBOOT_MS 5000
#endif

// ------------------------------
// Audio capture and encode
// ------------------------------
#ifndef CFG_AUDIO_SAMPLE_RATE
#define CFG_AUDIO_SAMPLE_RATE 16000
#endif

#ifndef CFG_AUDIO_BITS_PER_SAMPLE
#define CFG_AUDIO_BITS_PER_SAMPLE 16
#endif

#if CFG_AUDIO_BITS_PER_SAMPLE != 8 && CFG_AUDIO_BITS_PER_SAMPLE != 16 && \
    CFG_AUDIO_BITS_PER_SAMPLE != 24 && CFG_AUDIO_BITS_PER_SAMPLE != 32
#error "CFG_AUDIO_BITS_PER_SAMPLE must be 8, 16, 24, or 32"
#endif

#ifndef CFG_MP3_BITRATE_KBPS
#define CFG_MP3_BITRATE_KBPS 24
#endif

#ifndef CFG_I2S_BCLK_PIN
#define CFG_I2S_BCLK_PIN 6
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

#ifndef CFG_MIC_BUFFER_BYTES
#define CFG_MIC_BUFFER_BYTES 512
#endif

// One-shot menu sample length.
#ifndef CFG_SAMPLE_RECORD_MS
#define CFG_SAMPLE_RECORD_MS 5000
#endif

// ------------------------------
// Performance knobs for multicore recorder
// ------------------------------
// From prior measurements peak was ~255 samples at 16 kHz. Keep 2x+ headroom.
#ifndef CFG_PCM_RING_SAMPLES
#define CFG_PCM_RING_SAMPLES 1024
#endif

#ifndef CFG_RING_PUBLISH_SAMPLES
#define CFG_RING_PUBLISH_SAMPLES 64
#endif

#ifndef CFG_RING_CONSUME_SAMPLES
#define CFG_RING_CONSUME_SAMPLES 256
#endif

#ifndef CFG_MP3_STAGE_BYTES
#define CFG_MP3_STAGE_BYTES 1024
#endif

// Reserve heap headroom for encoder internals and filesystem operations.
#ifndef CFG_MP3_RAM_RESERVE_BYTES
#define CFG_MP3_RAM_RESERVE_BYTES 147456
#endif

#ifndef CFG_MP3_RAM_MIN_BYTES
#define CFG_MP3_RAM_MIN_BYTES 65536
#endif

#ifndef CFG_MP3_RAM_ALLOC_STEP_BYTES
#define CFG_MP3_RAM_ALLOC_STEP_BYTES 4096
#endif

// ------------------------------
// Storage
// ------------------------------
#ifndef CFG_SD_CS_PIN
#define CFG_SD_CS_PIN 17
#endif

#ifndef CFG_SD_SCK_PIN
#define CFG_SD_SCK_PIN 18
#endif

#ifndef CFG_SD_MISO_PIN
#define CFG_SD_MISO_PIN 16
#endif

#ifndef CFG_SD_MOSI_PIN
#define CFG_SD_MOSI_PIN 19
#endif

#ifndef CFG_SD_SPI_MHZ
#define CFG_SD_SPI_MHZ 10
#endif

#ifndef CFG_STORAGE_MARGIN_BYTES
#define CFG_STORAGE_MARGIN_BYTES 2048
#endif

// ------------------------------
// RGB LED status output
// ------------------------------
#ifndef CFG_LED_ENABLE
#define CFG_LED_ENABLE 1
#endif

#ifndef CFG_LED_R_PIN
#define CFG_LED_R_PIN 2
#endif

#ifndef CFG_LED_G_PIN
#define CFG_LED_G_PIN 3
#endif

#ifndef CFG_LED_B_PIN
#define CFG_LED_B_PIN 4
#endif

// Set to 1 for active-low LEDs.
#ifndef CFG_LED_INVERT_LOGIC
#define CFG_LED_INVERT_LOGIC 0
#endif

// ------------------------------
// Battery monitoring (RP2040-Battery-Check formula)
// ------------------------------
#ifndef CFG_BATTERY_ADC_GPIO
#define CFG_BATTERY_ADC_GPIO 29
#endif

#ifndef CFG_BATTERY_ADC_INPUT
#define CFG_BATTERY_ADC_INPUT 3
#endif

#ifndef CFG_BATTERY_LOW_THRESHOLD_MV
#define CFG_BATTERY_LOW_THRESHOLD_MV 3250
#endif

#ifndef CFG_BATTERY_RECHARGE_DELTA_MV
#define CFG_BATTERY_RECHARGE_DELTA_MV 200
#endif

#ifndef CFG_BATTERY_CHECK_INTERVAL_MS
#define CFG_BATTERY_CHECK_INTERVAL_MS 15000
#endif

// ------------------------------
// Persistent settings defaults
// ------------------------------
#ifndef CFG_EEPROM_BYTES
#define CFG_EEPROM_BYTES 512
#endif

#ifndef CFG_SETTINGS_MAGIC
#define CFG_SETTINGS_MAGIC 0x50525244u
#endif

#ifndef CFG_SETTINGS_VERSION
#define CFG_SETTINGS_VERSION 1u
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
