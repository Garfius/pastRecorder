#ifdef PASTRECORDER_HOST_TEST

#include "../LameEncoder.h"
#include "../DeviceSettings.h"
#include "../VoiceCompressor.h"

#include <assert.h>
#include <stdio.h>
#include <thread>
#include <vector>

#if USE_STACK_HACK
#include <liblame/stackhack.h>
#endif

namespace {
#if USE_STACK_HACK
void testLameScratch() {
  stackhack_setup(STACKHACK_SIZE);
  auto *persistent = static_cast<uint8_t *>(stackhack_alloc(3));
  memset(persistent, 0xA5, 3);
  stackhack_end();
  stackhack_setup(STACKHACK_SIZE);
  auto *temporary = static_cast<uint8_t *>(stackhack_alloc(17));
  assert(reinterpret_cast<uintptr_t>(persistent) % alignof(max_align_t) == 0);
  assert(reinterpret_cast<uintptr_t>(temporary) % alignof(max_align_t) == 0);
  assert(temporary >= persistent + 3);
  for (size_t index = 0; index < 3; ++index) {
    assert(persistent[index] == 0xA5);
  }
  for (size_t index = 0; index < 17; ++index) {
    assert(temporary[index] == 0);
  }
  stackhack_free(persistent);
  auto *nested = static_cast<uint8_t *>(stackhack_alloc(5));
  assert(nested >= temporary + 17);
  memset(temporary, 0x5A, 17);
  stackhack_free(nested);
  for (size_t index = 0; index < 17; ++index) {
    assert(temporary[index] == 0x5A);
  }
  stackhack_free(temporary);
  auto *whole = static_cast<uint8_t *>(stackhack_alloc(STACKHACK_SIZE));
  assert(whole == persistent);
  assert(whole[0] == 0 && whole[STACKHACK_SIZE - 1] == 0);
  stackhack_free(whole);
  stackhack_end();
}
#endif

std::vector<uint8_t> snapshot(const Mp3FrameRing &ring) {
  std::vector<uint8_t> result;
  for (size_t offset = 0; offset < ring.size();) {
    size_t count;
    const uint8_t *bytes = ring.segment(offset, count);
    result.insert(result.end(), bytes, bytes + count);
    offset += count;
  }
  return result;
}

void testFrameRing() {
  uint8_t memory[kMp3FrameBytes * 3 + 7];
  uint8_t frame[kMp3FrameBytes] = {0xFF, 0xF3, 0x38, 0xC4, 0};
  Mp3FrameRing ring;
  assert(!ring.append(frame, sizeof(frame)));
  ring.attach(memory, sizeof(memory));
  assert(ring.capacity() == kMp3FrameBytes * 3);
  assert(ring.size() == 0 && ring.complete());
  for (unsigned i = 0; i < 10; ++i) {
    frame[8] = i;
    assert(ring.append(frame, 7));
    assert(!ring.complete());
    assert(ring.append(frame + 7, sizeof(frame) - 7));
    assert(ring.complete());
  }
  assert(ring.framesWritten() == 10);
  assert(ring.size() == kMp3FrameBytes * 3);
  assert(ring.durationMs() == 108);
  const auto bytes = snapshot(ring);
  for (unsigned i = 0; i < 3; ++i) {
    assert(bytes[i * kMp3FrameBytes + 8] == i + 7);
  }
  size_t count = 999;
  assert(!ring.segment(ring.size(), count) && count == 0);
  ring.reset();
  assert(ring.framesWritten() == 0 && ring.size() == 0);
  frame[4] = 1;
  assert(!ring.append(frame, sizeof(frame))); // Reservoir references forbidden.
  ring.reset();
  frame[4] = 0;
  frame[2] |= 2;
  assert(!ring.append(frame, sizeof(frame))); // Padding forbidden.
  ring.attach(memory, kMp3FrameBytes - 1);
  assert(!ring.append(frame, sizeof(frame)));
}

void testPcmRing() {
  PcmRing ring;
  for (size_t i = 0; i < CFG_PCM_RING_SAMPLES; ++i) {
    assert(ring.push(static_cast<int16_t>(i)));
  }
  assert(!ring.push(1));
  assert(ring.peak() == CFG_PCM_RING_SAMPLES);
  int16_t block[CFG_PCM_RING_SAMPLES];
  assert(ring.pop(block, CFG_PCM_RING_SAMPLES) == CFG_PCM_RING_SAMPLES);
  for (size_t i = 0; i < CFG_PCM_RING_SAMPLES; ++i) {
    assert(block[i] == static_cast<int16_t>(i));
  }
  ring.reset();
  assert(ring.peak() == 0 && ring.pop(block, 1) == 0);
  constexpr uint32_t samples = 1000000;
  std::thread producer([&] {
    for (uint32_t i = 0; i < samples; ++i) {
      while (!ring.push(static_cast<int16_t>(i))) {
        std::this_thread::yield();
      }
    }
  });
  uint32_t received = 0;
  while (received < samples) {
    const size_t n = ring.pop(block, CFG_PCM_RING_SAMPLES);
    for (size_t i = 0; i < n; ++i) {
      assert(block[i] == static_cast<int16_t>(received++));
    }
  }
  producer.join();
  assert(received == samples);
}

void testSettingsAndCompressor() {
  DeviceSettings settings;
  settings.seal();
  assert(settings.valid());
  settings.lowBatteryShutdown = 1;
  assert(!settings.valid());
  settings.seal();
  assert(settings.valid());
  settings.audio.inputGain = NAN;
  settings.seal();
  assert(!settings.valid());
  settings = DeviceSettings{};
  settings.audio.ratio = 0;
  settings.seal();
  assert(!settings.valid());
  settings = DeviceSettings{};
  settings.version++;
  settings.seal();
  assert(!settings.valid());
  settings = DeviceSettings{};
  settings.lowBatteryShutdown = 2;
  settings.seal();
  assert(!settings.valid());
  settings = DeviceSettings{};
  VoiceCompressor compressor;
  compressor.begin(settings.audio);
  assert(compressor.process(0) == 0);
  for (unsigned i = 0; i < 10000; ++i) {
    const int32_t sample = i & 1 ? INT32_MIN : INT32_MAX;
    const int16_t result = compressor.process(sample);
    assert(abs(static_cast<int>(result)) <= settings.audio.ceiling * 32767 + 1);
  }
  const uint8_t text[] = "123456789";
  assert(~crc32Update(0xFFFFFFFFu, text, 9) == 0xCBF43926u);
}

void writeFile(const char *path, const std::vector<uint8_t> &bytes) {
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  assert(fclose(file) == 0);
}

void testRealEncoder(const char *path) {
  std::vector<uint8_t> fullMemory(kMp3FrameBytes * 1000);
  std::vector<uint8_t> smallMemory(kMp3FrameBytes * 64);
  Mp3FrameRing full;
  Mp3FrameRing wrapped;
  full.attach(fullMemory.data(), fullMemory.size());
  wrapped.attach(smallMemory.data(), smallMemory.size());
  LameEncoder encoder;
  assert(encoder.warmUp());
  assert(encoder.begin(full));
  int16_t pcm[CFG_PCM_BLOCK_SAMPLES];
  for (size_t i = 0; i < CFG_AUDIO_SAMPLE_RATE * 5;) {
    const size_t left = CFG_AUDIO_SAMPLE_RATE * 5 - i;
    const size_t n = left < CFG_PCM_BLOCK_SAMPLES ? left : CFG_PCM_BLOCK_SAMPLES;
    for (size_t j = 0; j < n; ++j) {
      pcm[j] = static_cast<int16_t>(
          12000 * sin(6.28318530718 * 440 * (i + j) / CFG_AUDIO_SAMPLE_RATE));
    }
    assert(encoder.write(pcm, n));
    i += n;
  }
  const size_t beforeFlush = full.size();
  assert(encoder.finish());
  assert(full.complete());
  assert(full.size() > beforeFlush); // Tail must not disappear on stop.
  assert(full.framesWritten() >= 139 && full.framesWritten() <= 143);
  const auto encoded = snapshot(full);
  // Match a real encoder's arbitrarily fragmented writes across many wraps.
  for (size_t offset = 0; offset < encoded.size();) {
    const size_t n = encoded.size() - offset < 79 ? encoded.size() - offset : 79;
    assert(wrapped.append(encoded.data() + offset, n));
    offset += n;
  }
  const auto tail = snapshot(wrapped);
  assert(tail.size() == smallMemory.size());
  assert(memcmp(tail.data(), encoded.data() + encoded.size() - tail.size(),
                tail.size()) == 0);
  assert(wrapped.complete());
  assert(wrapped.durationMs() == 2304);
  writeFile(path, tail);
  printf("5-second source: %zu bytes, %llu frames; wrapped tail: %zu bytes, "
         "%u ms; flush added %zu bytes\n",
         encoded.size(), static_cast<unsigned long long>(full.framesWritten()),
         tail.size(), wrapped.durationMs(), full.size() - beforeFlush);

  // Repeated fresh sessions must not retain old bytes or use freed LAME state.
  for (unsigned session = 0; session < 10; ++session) {
    full.reset();
    assert(encoder.warmUp());
    assert(encoder.begin(full));
    assert(encoder.write(pcm, 17));
    assert(encoder.finish());
    assert(full.complete() && full.size() > 0);
  }
  full.reset();
  assert(encoder.begin(full));
  assert(encoder.finish()); // Stop before the first microphone sample.
  assert(full.complete());
}
}

int main(int argc, char **argv) {
  assert(argc == 2);
#if USE_STACK_HACK
  testLameScratch();
#endif
  testFrameRing();
  testPcmRing();
  testSettingsAndCompressor();
  testRealEncoder(argv[1]);
  puts("All recorder tests passed.");
}

#endif
