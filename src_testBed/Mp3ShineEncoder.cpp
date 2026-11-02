#include "Mp3ShineEncoder.h"

#include <codec-shine.h>
#include <stdlib.h>
#include <string.h>

Mp3ShineEncoder::Mp3ShineEncoder(Print &output, uint32_t sampleRate,
                               uint32_t bitrateKbps)
    : output_(output), sampleRate_(sampleRate), bitrateKbps_(bitrateKbps) {}

Mp3ShineEncoder::~Mp3ShineEncoder() {
  end();
}

bool Mp3ShineEncoder::begin() {
  end();
  failed_ = false;
  bufferedBytes_ = 0;
  if (sampleRate_ > 48000 || bitrateKbps_ > 320 ||
      shine_check_config(sampleRate_, bitrateKbps_) < 0) {
    return false;
  }

  shine_config_t config{};
  shine_set_config_mpeg_defaults(&config.mpeg);
  config.wave.channels = PCM_MONO;
  config.wave.samplerate = sampleRate_;
  config.mpeg.mode = MONO;
  config.mpeg.bitr = bitrateKbps_;
  encoder_ = shine_initialise(&config);
  if (!encoder_) {
    return false;
  }
  if (!encoder_->bs.data) {
    shine_close(encoder_);
    encoder_ = nullptr;
    return false;
  }

  const uint32_t frameSamples = shine_samples_per_pass(encoder_);
  frameBytes_ = frameSamples * sizeof(int16_t);
  pcm_ = static_cast<int16_t *>(malloc(frameBytes_));
  if (!pcm_) {
    shine_close(encoder_);
    encoder_ = nullptr;
    return false;
  }
  const uint32_t numerator = frameSamples * bitrateKbps_ * 1000UL;
  const uint32_t denominator = sampleRate_ * 8UL;
  encoder_->mpeg.whole_slots_per_frame = numerator / denominator;
  encoder_->mpeg.frac_slots_per_frame =
      static_cast<double>(numerator % denominator) / denominator;
  encoder_->mpeg.slot_lag = -encoder_->mpeg.frac_slots_per_frame;
  encoder_->mpeg.padding = 0;
  return true;
}

size_t Mp3ShineEncoder::write(const uint8_t *data, size_t length) {
  if (!encoder_ || failed_ || (!data && length > 0)) {
    return 0;
  }

  size_t consumed = 0;
  while (consumed < length) {
    const size_t room = frameBytes_ - bufferedBytes_;
    const size_t pending = length - consumed;
    const size_t copied = pending < room ? pending : room;
    memcpy(reinterpret_cast<uint8_t *>(pcm_) + bufferedBytes_, data + consumed,
           copied);
    bufferedBytes_ += copied;
    consumed += copied;
    if (bufferedBytes_ == frameBytes_ && !writeFrame()) {
      return 0;
    }
  }
  return consumed;
}

bool Mp3ShineEncoder::writeOutput(const uint8_t *data, int length) {
  if (length < 0 || (length > 0 &&
                     (!data || output_.write(data, length) !=
                                   static_cast<size_t>(length)))) {
    failed_ = true;
  }
  return !failed_;
}

bool Mp3ShineEncoder::writeFrame() {
  int written = 0;
  uint8_t *encoded = shine_encode_buffer_interleaved(encoder_, pcm_, &written);
  bufferedBytes_ = 0;
  return writeOutput(encoded, written);
}

bool Mp3ShineEncoder::end() {
  if (encoder_) {
    if (bufferedBytes_ % sizeof(int16_t) != 0) {
      failed_ = true;
    }
    if (!failed_ && bufferedBytes_ > 0) {
      memset(reinterpret_cast<uint8_t *>(pcm_) + bufferedBytes_, 0,
             frameBytes_ - bufferedBytes_);
      writeFrame();
    }
    if (!failed_) {
      int written = 0;
      uint8_t *encoded = shine_flush(encoder_, &written);
      writeOutput(encoded, written);
      if (!failed_) {
        uint8_t cached[4];
        const int cachedBytes = (32 - encoder_->bs.cache_bits) / 8;
        for (int byteIndex = 0; byteIndex < cachedBytes; ++byteIndex) {
          cached[byteIndex] = static_cast<uint8_t>(
              encoder_->bs.cache >> (24 - byteIndex * 8));
        }
        writeOutput(cached, cachedBytes);
      }
    }
    shine_close(encoder_);
    encoder_ = nullptr;
    free(pcm_);
    pcm_ = nullptr;
    bufferedBytes_ = 0;
  }
  return !failed_;
}