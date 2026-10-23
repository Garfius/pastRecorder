#pragma once

#include <Arduino.h>

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 16000
#endif

#ifndef AUDIO_BITS_PER_SAMPLE
#define AUDIO_BITS_PER_SAMPLE 16
#endif

#if AUDIO_BITS_PER_SAMPLE != 8 && AUDIO_BITS_PER_SAMPLE != 16 && \
    AUDIO_BITS_PER_SAMPLE != 24 && AUDIO_BITS_PER_SAMPLE != 32
#error "AUDIO_BITS_PER_SAMPLE must be 8, 16, 24, or 32"
#endif

#if defined(rp2040_zero) || defined(rp2350_zero) || defined(PICO_RP2040) || \
    defined(PICO_RP2350)
constexpr uint8_t kMicBclkPin = 6;
constexpr uint8_t kMicDataPin = 10;
#else
#error "Select an RP2040 or RP2350 board using the PlatformIO build flags"
#endif

constexpr bool kAudioHighQuality = false;
constexpr uint32_t kSampleRate = AUDIO_SAMPLE_RATE;
constexpr uint32_t kRecordSeconds = 5;
constexpr uint32_t kSampleCount = kSampleRate * kRecordSeconds;
constexpr uint32_t kWavHeaderSize = 44;
constexpr uint8_t kWavSampleBytes = AUDIO_BITS_PER_SAMPLE / 8;
constexpr size_t kMp3PcmBufferSize = 1024;
constexpr bool kMicRightChannel = false;
constexpr char kRecordingPath[] = "/voice.wav";
constexpr char kMp3RecordingPath[] = "/voice.mp3";

constexpr uint32_t selectMp3SampleRate() {
  constexpr uint32_t supportedSampleRates[] = {
      8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
  uint32_t closestSampleRate = supportedSampleRates[0];
  uint32_t closestDifference = kSampleRate > closestSampleRate
                                   ? kSampleRate - closestSampleRate
                                   : closestSampleRate - kSampleRate;
  for (size_t index = 1;
       index < sizeof(supportedSampleRates) / sizeof(supportedSampleRates[0]);
       ++index) {
    const uint32_t sampleRate = supportedSampleRates[index];
    const uint32_t difference = sampleRate > kSampleRate
                                    ? sampleRate - kSampleRate
                                    : kSampleRate - sampleRate;
    if (difference < closestDifference) {
      closestSampleRate = sampleRate;
      closestDifference = difference;
    }
  }
  return closestSampleRate;
}

constexpr uint32_t kMp3SampleRate = selectMp3SampleRate();

constexpr uint32_t selectMp3BitrateKbps() {
  constexpr uint32_t suggestedSampleRates[] = {8000, 16000, 22050, 32000,
                                                44100};
  constexpr uint32_t standardBitratesKbps[] = {16, 24, 32, 64, 96};
  constexpr uint32_t highQualityBitratesKbps[] = {32, 64, 80, 128, 192};

  size_t closestIndex = 0;
  uint32_t closestDifference = kSampleRate > suggestedSampleRates[0]
                                   ? kSampleRate - suggestedSampleRates[0]
                                   : suggestedSampleRates[0] - kSampleRate;
  for (size_t index = 1;
       index < sizeof(suggestedSampleRates) / sizeof(suggestedSampleRates[0]);
       ++index) {
    const uint32_t sampleRate = suggestedSampleRates[index];
    const uint32_t difference = sampleRate > kSampleRate
                                    ? sampleRate - kSampleRate
                                    : kSampleRate - sampleRate;
    if (difference < closestDifference) {
      closestIndex = index;
      closestDifference = difference;
    }
  }
  return kAudioHighQuality ? highQualityBitratesKbps[closestIndex]
                           : standardBitratesKbps[closestIndex];
}

constexpr uint32_t kMp3BitrateKbps = selectMp3BitrateKbps();
constexpr uint32_t kMp3EstimatedSize =
    (kMp3BitrateKbps * 1000UL * kRecordSeconds) / 8 + 4096;