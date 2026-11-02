#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"

// This fixed format has exactly 108 bytes per MPEG-2 Layer III mono frame.
static_assert(CFG_AUDIO_SAMPLE_RATE == 16000 && CFG_MP3_BITRATE_KBPS == 24,
              "The frame ring is specialized for 16 kHz, 24 kbps mono CBR");
constexpr size_t kMp3FrameBytes = 72 * CFG_MP3_BITRATE_KBPS * 1000 /
                                CFG_AUDIO_SAMPLE_RATE;
constexpr size_t kMp3FrameSamples = 576;

inline uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t size) {
  while (size--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

class Mp3FrameRing {
 public:
  void attach(uint8_t *memory, size_t bytes) {
    data_ = memory;
    capacity_ = bytes / kMp3FrameBytes * kMp3FrameBytes;
    reset();
  }

  void reset() {
    head_ = size_ = partialSize_ = 0;
    framesWritten_ = 0;
    failed_ = false;
  }

  bool append(const uint8_t *bytes, size_t count) {
    if (!data_ || capacity_ < kMp3FrameBytes || failed_) {
      failed_ = true;
      return false;
    }
    while (count) {
      const size_t space = kMp3FrameBytes - partialSize_;
      const size_t n = count < space ? count : space;
      memcpy(partial_ + partialSize_, bytes, n);
      bytes += n;
      count -= n;
      partialSize_ += n;
      if (partialSize_ != kMp3FrameBytes) {
        continue;
      }
      // No ID3/Xing tags, CRC, padding, stereo or backward reservoir references.
      if (partial_[0] != 0xFF || partial_[1] != 0xF3 ||
          (partial_[2] & 0xFE) != 0x38 ||
          (partial_[3] & 0xC0) != 0xC0 || partial_[4] != 0) {
        failed_ = true;
        return false;
      }
      memcpy(data_ + head_, partial_, kMp3FrameBytes);
      head_ = (head_ + kMp3FrameBytes) % capacity_;
      if (size_ < capacity_) {
        size_ += kMp3FrameBytes;
      }
      ++framesWritten_;
      partialSize_ = 0;
    }
    return true;
  }

  bool complete() const { return !failed_ && partialSize_ == 0; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  uint64_t framesWritten() const { return framesWritten_; }
  uint32_t durationMs() const {
    return size_ / kMp3FrameBytes * kMp3FrameSamples * 1000 /
           CFG_AUDIO_SAMPLE_RATE;
  }

  // Only inspect on core 0 after the encoder has acknowledged stopping.
  const uint8_t *segment(size_t offset, size_t &length) const {
    if (offset >= size_) {
      length = 0;
      return nullptr;
    }
    const size_t oldest = size_ == capacity_ ? head_ : 0;
    const size_t start = (oldest + offset) % capacity_;
    length = capacity_ - start;
    if (length > size_ - offset) {
      length = size_ - offset;
    }
    return data_ + start;
  }

 private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
  size_t head_ = 0;
  size_t size_ = 0;
  uint64_t framesWritten_ = 0;
  uint8_t partial_[kMp3FrameBytes] = {};
  size_t partialSize_ = 0;
  bool failed_ = false;
};

class PcmRing {
 public:
  static_assert(CFG_PCM_RING_SAMPLES >= 512 &&
                    (CFG_PCM_RING_SAMPLES & (CFG_PCM_RING_SAMPLES - 1)) == 0,
                "PCM ring must be a power of two, at least twice measured peak");
  static_assert(CFG_PCM_BLOCK_SAMPLES > 0 &&
                    CFG_PCM_BLOCK_SAMPLES <= CFG_PCM_RING_SAMPLES,
                "PCM blocks must fit in the ring");

  void reset() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    peak_ = 0;
  }

  bool push(int16_t value) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t queued = head - tail_.load(std::memory_order_acquire);
    if (queued == CFG_PCM_RING_SAMPLES) {
      return false;
    }
    data_[head & (CFG_PCM_RING_SAMPLES - 1)] = value;
    if (queued + 1 > peak_) {
      peak_ = queued + 1;
    }
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  size_t pop(int16_t *out, size_t limit) {
    uint32_t tail = tail_.load(std::memory_order_relaxed);
    size_t count = head_.load(std::memory_order_acquire) - tail;
    if (count > limit) {
      count = limit;
    }
    for (size_t i = 0; i < count; ++i) {
      out[i] = data_[tail++ & (CFG_PCM_RING_SAMPLES - 1)];
    }
    tail_.store(tail, std::memory_order_release);
    return count;
  }

  uint32_t peak() const { return peak_; }

 private:
  int16_t data_[CFG_PCM_RING_SAMPLES] = {};
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
  uint32_t peak_ = 0; // Producer-owned; inspected only after capture stops.
};
