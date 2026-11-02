#pragma once

#include <Print.h>
#include <stddef.h>
#include <stdint.h>

struct shine_global_flags;

// Encodes mono signed 16-bit PCM, buffering arbitrary writes into Shine frames.
class Mp3ShineEncoder {
 public:
  Mp3ShineEncoder(Print &output, uint32_t sampleRate, uint32_t bitrateKbps);
  ~Mp3ShineEncoder();
  Mp3ShineEncoder(const Mp3ShineEncoder &) = delete;
  Mp3ShineEncoder &operator=(const Mp3ShineEncoder &) = delete;

  bool begin();
  size_t write(const uint8_t *data, size_t length);
  bool end();

 private:
  bool writeFrame();
  bool writeOutput(const uint8_t *data, int length);

  Print &output_;
  const uint32_t sampleRate_;
  const uint32_t bitrateKbps_;
  shine_global_flags *encoder_ = nullptr;
  int16_t *pcm_ = nullptr;
  size_t frameBytes_ = 0;
  size_t bufferedBytes_ = 0;
  bool failed_ = false;
};
