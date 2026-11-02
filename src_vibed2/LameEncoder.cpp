#include "LameEncoder.h"

#if USE_STACK_HACK
#include <liblame/stackhack.h>
#include <stdlib.h>

namespace {
alignas(max_align_t) uint8_t lameScratch[STACKHACK_SIZE] = {};
struct ScratchAllocation {
  size_t start;
  size_t end;
  bool released;
};
ScratchAllocation scratchAllocations[32] = {};
size_t scratchUsed = 0;
size_t scratchAllocationCount = 0;
}

extern "C" void stackhack_setup(int size) {
  if (size <= 0 || static_cast<size_t>(size) > sizeof(lameScratch)) {
    abort();
  }
}

extern "C" void stackhack_end() {}

extern "C" void *stackhack_alloc(int size) {
  constexpr size_t alignment = alignof(max_align_t);
  const size_t start = (scratchUsed + alignment - 1) / alignment * alignment;
  if (size <= 0 || start > sizeof(lameScratch) ||
      static_cast<size_t>(size) > sizeof(lameScratch) - start ||
      scratchAllocationCount ==
          sizeof(scratchAllocations) / sizeof(scratchAllocations[0])) {
    abort();
  }
  const size_t end = start + static_cast<size_t>(size);
  scratchAllocations[scratchAllocationCount++] = {start, end, false};
  scratchUsed = end;
  void *result = lameScratch + start;
  memset(result, 0, static_cast<size_t>(size));
  return result;
}

extern "C" void stackhack_free(void *memory) {
  size_t index = scratchAllocationCount;
  while (index && lameScratch + scratchAllocations[index - 1].start != memory) {
    --index;
  }
  if (!index || scratchAllocations[index - 1].released) {
    abort();
  }
  scratchAllocations[index - 1].released = true;
  while (scratchAllocationCount &&
         scratchAllocations[scratchAllocationCount - 1].released) {
    --scratchAllocationCount;
  }
  scratchUsed = scratchAllocationCount ?
      scratchAllocations[scratchAllocationCount - 1].end : 0;
}
#endif

bool LameEncoder::begin(Mp3FrameRing &sink) {
  close();
  sink_ = &sink;
  encoder_ = lame_init();
  if (!encoder_) {
    return false;
  }
  const bool configured =
      lame_set_VBR(encoder_, vbr_off) == 0 &&
      lame_set_brate(encoder_, CFG_MP3_BITRATE_KBPS) == 0 &&
      lame_set_num_channels(encoder_, 1) == 0 &&
      lame_set_mode(encoder_, MONO) == 0 &&
      lame_set_in_samplerate(encoder_, CFG_AUDIO_SAMPLE_RATE) == 0 &&
      lame_set_out_samplerate(encoder_, CFG_AUDIO_SAMPLE_RATE) == 0 &&
      lame_set_quality(encoder_, CFG_MP3_LAME_QUALITY) == 0 &&
      lame_set_disable_reservoir(encoder_, 1) == 0 &&
      lame_set_bWriteVbrTag(encoder_, 0) == 0 &&
      lame_set_error_protection(encoder_, 0) == 0;
  lame_set_write_id3tag_automatic(encoder_, 0);
  if (!configured || lame_init_params(encoder_) < 0 ||
      lame_get_framesize(encoder_) != kMp3FrameSamples) {
    close();
    return false;
  }
  return true;
}

bool LameEncoder::accept(int bytes) {
  return bytes >= 0 &&
         (bytes == 0 || sink_->append(output_, static_cast<size_t>(bytes)));
}

bool LameEncoder::write(const int16_t *samples, size_t count) {
  if (!encoder_ || count > CFG_PCM_BLOCK_SAMPLES) {
    return false;
  }
  return accept(lame_encode_buffer(encoder_, samples, nullptr,
                                  static_cast<int>(count), output_,
                                  sizeof(output_)));
}

bool LameEncoder::finish() {
  if (!encoder_) {
    return false;
  }
  const bool ok =
      accept(lame_encode_flush(encoder_, output_, sizeof(output_))) &&
      sink_->complete();
  close();
  return ok;
}

void LameEncoder::close() {
  if (encoder_) {
    lame_close(encoder_);
    encoder_ = nullptr;
  }
}

bool LameEncoder::warmUp() {
  // Force LAME's lazy PCM/frame allocations before sizing the large MP3 ring.
  uint8_t scratch[kMp3FrameBytes * 8];
  Mp3FrameRing discard;
  discard.attach(scratch, sizeof(scratch));
  int16_t silence[CFG_PCM_BLOCK_SAMPLES] = {};
  if (!begin(discard)) {
    return false;
  }
  bool ok = true;
  for (size_t n = 0; n < 4 * kMp3FrameSamples && ok;
       n += CFG_PCM_BLOCK_SAMPLES) {
    ok = write(silence, CFG_PCM_BLOCK_SAMPLES);
  }
  const bool flushed = finish();
  return ok && flushed;
}
