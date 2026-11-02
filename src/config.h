#pragma once

// ------------------------------
// Serial and user interaction
// ------------------------------
#define CFG_SERIAL_PORT Serial1
#define CFG_SERIAL_BAUD 115200

#define CFG_BUTTON_PIN 9
#define CFG_BUTTON_DEBOUNCE_MS 30

// Debounced button events and save/shutdown diagnostics use CFG_SERIAL_PORT.
// Button long press powers off without setting low-battery shutdown state.
#define CFG_LONG_PRESS_POWER_OFF_MS 2500

#define CFG_MENU_EXIT_REBOOT_MS 2000

// ------------------------------
// Audio capture and encode
// ------------------------------
#define CFG_AUDIO_SAMPLE_RATE 16000

#define CFG_AUDIO_BITS_PER_SAMPLE 16

#if CFG_AUDIO_BITS_PER_SAMPLE != 8 && CFG_AUDIO_BITS_PER_SAMPLE != 16 && \
    CFG_AUDIO_BITS_PER_SAMPLE != 24 && CFG_AUDIO_BITS_PER_SAMPLE != 32
#error "CFG_AUDIO_BITS_PER_SAMPLE must be 8, 16, 24, or 32"
#endif

#define CFG_MP3_BITRATE_KBPS 24

#define CFG_I2S_BCLK_PIN 6

#define CFG_I2S_DATA_PIN 10

#define CFG_I2S_USE_RIGHT_CHANNEL 0

#define CFG_MIC_BUFFER_COUNT 6

#define CFG_MIC_BUFFER_BYTES 512

// One-shot menu sample length.
#define CFG_SAMPLE_RECORD_MS 5000

// ------------------------------
// Performance knobs for multicore recorder
// ------------------------------
// From prior measurements peak was ~255 samples at 16 kHz. Keep 2x+ headroom.
#define CFG_PCM_RING_SAMPLES 1024

#define CFG_RING_PUBLISH_SAMPLES 64

#define CFG_RING_CONSUME_SAMPLES 256

#define CFG_MP3_STAGE_BYTES 1024

// Maximum wait for core1 encoder worker to acknowledge stop.
#define CFG_ENCODER_STOP_TIMEOUT_MS 3000

// Maximum wait for core1 to reset the encoder and acknowledge startup.
#define CFG_ENCODER_START_TIMEOUT_MS 3000

// Operational headroom AFTER Shine and I2S allocations, not including them.
// 16 KiB reclaims 16 KiB (~5.46 seconds at 24 kbps) versus the 32 KiB reserve.
// Check the observed heap during storage/menu use before reducing this further.
#define CFG_MP3_RAM_RESERVE_BYTES 16384

#define CFG_MP3_RAM_MIN_BYTES 65536

#define CFG_MP3_RAM_ALLOC_STEP_BYTES 4096

// If recorder start fails, reallocate a smaller MP3 ring once and retry.
#define CFG_MP3_RAM_RECOVERY_EXTRA_RESERVE_BYTES 65536

// Do not shrink below this ring size during recovery.
#define CFG_MP3_RAM_RECOVERY_MIN_BYTES 32768

// ------------------------------
// Storage
// ------------------------------
#define CFG_SD_CS_PIN 5

#define CFG_SD_SCK_PIN 2

#define CFG_SD_MISO_PIN 4

#define CFG_SD_MOSI_PIN 3

#define CFG_SD_SPI_MHZ 10

#define CFG_STORAGE_MARGIN_BYTES 2048

// ------------------------------
// RGB LED status output
// ------------------------------
#define CFG_LED_ENABLE 1

#define CFG_LED_R_PIN 11

#define CFG_LED_G_PIN 12

#define CFG_LED_B_PIN 13

// Set to 1 for active-low LEDs.
#define CFG_LED_INVERT_LOGIC 1

// ------------------------------
// Battery monitoring (RP2040-Battery-Check formula)
// ------------------------------
#define CFG_BATTERY_ADC_GPIO 29

#define CFG_BATTERY_ADC_INPUT 3

#define CFG_BATTERY_LOW_THRESHOLD_MV 350

#define CFG_BATTERY_RECHARGE_DELTA_MV 100

#define CFG_BATTERY_CHECK_INTERVAL_MS 15000

// ------------------------------
// Persistent settings defaults
// ------------------------------
#define CFG_EEPROM_BYTES 512

#define CFG_SETTINGS_MAGIC 0x50525244u

#define CFG_SETTINGS_VERSION 1u

#define CFG_DEFAULT_HIGHPASS_HZ 120.0f

#define CFG_DEFAULT_INPUT_GAIN 2.0f

#define CFG_DEFAULT_MAKEUP_GAIN 2.0f

#define CFG_DEFAULT_CEILING 0.999f

#define CFG_DEFAULT_ATTACK_MS 10.0f

#define CFG_DEFAULT_RELEASE_MS 200.0f

#define CFG_DEFAULT_THRESHOLD 0.1f

#define CFG_DEFAULT_RATIO 8.0f
