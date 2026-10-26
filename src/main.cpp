#include <Arduino.h>
#include <I2S.h>
#include <LittleFS.h>
#include <XModem.h>
#include <AudioTools.h>
#include <AudioTools/AudioCodecs/CodecMP3Shine.h>
#include <pico/sync.h>
#include <malloc.h>
#include <new>
#include <string.h>
#include "VoiceCompressor.h"

#define serialPortUsed Serial1

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 16000// 16 kHz or 48 kHz are common stable targets for this mic.
#endif

#ifndef AUDIO_BITS_PER_SAMPLE
#define AUDIO_BITS_PER_SAMPLE 16
#endif

constexpr bool AUDIO_HQ = false;

#if AUDIO_BITS_PER_SAMPLE != 8 && AUDIO_BITS_PER_SAMPLE != 16 && \
  AUDIO_BITS_PER_SAMPLE != 24 && AUDIO_BITS_PER_SAMPLE != 32
#error "AUDIO_BITS_PER_SAMPLE must be 8, 16, 24, or 32"
#endif

#if defined(rp2040_zero)
constexpr uint8_t kMicBclkPin = 6;
constexpr uint8_t kMicDataPin = 10;
#elif defined(rp2350_zero)
constexpr uint8_t kMicBclkPin = 6;
constexpr uint8_t kMicDataPin = 10;
#elif defined(PICO_RP2040) || defined(PICO_RP2350)
constexpr uint8_t kMicBclkPin = 6;
constexpr uint8_t kMicDataPin = 10;
#else
#error "Select an RP2040 or RP2350 board using the PlatformIO build flags"
#endif

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
  return AUDIO_HQ ? highQualityBitratesKbps[closestIndex]
                  : standardBitratesKbps[closestIndex];
}

constexpr uint32_t kMp3BitrateKbps = selectMp3BitrateKbps();
constexpr uint32_t kMp3EstimatedSize =
    (kMp3BitrateKbps * 1000UL * kRecordSeconds) / 8 + 4096;

constexpr char kRamMp3RecordingPath[] = "/voice_ram.mp3";
// MP3EncoderShine snaps requested bitrates to a 32..320 kbps ladder.
constexpr uint32_t kMp3RamBitrateKbps =
    kMp3BitrateKbps > 32 ? kMp3BitrateKbps : 32;
constexpr size_t kMp3RamBufferSize =
    (kMp3RamBitrateKbps * 1000UL * kRecordSeconds) / 8 + 4096;
constexpr uint32_t kPcmRingSamples = 16384;  // power of two, ~1 s at 16 kHz
constexpr uint32_t kPcmRingMask = kPcmRingSamples - 1;
constexpr uint32_t kRingPublishSamples = 64;  // power of two
constexpr uint32_t kRingConsumeSamples = 256;
static_assert((kPcmRingSamples & (kPcmRingSamples - 1)) == 0,
              "kPcmRingSamples must be a power of two");
static_assert((kRingPublishSamples & (kRingPublishSamples - 1)) == 0,
              "kRingPublishSamples must be a power of two");

I2S microphone(INPUT);
bool fileSystemReady = false;
bool xmodemReady = false;

bool onXmodemUpdate(uint8_t, uint8_t) {
  return true;
}

void writeLittleEndian16(uint8_t *buffer, size_t offset, uint16_t value) {
  buffer[offset] = static_cast<uint8_t>(value);
  buffer[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLittleEndian32(uint8_t *buffer, size_t offset, uint32_t value) {
  buffer[offset] = static_cast<uint8_t>(value);
  buffer[offset + 1] = static_cast<uint8_t>(value >> 8);
  buffer[offset + 2] = static_cast<uint8_t>(value >> 16);
  buffer[offset + 3] = static_cast<uint8_t>(value >> 24);
}

bool writeWavHeader(fs::File &file, uint32_t dataSize) {
  uint8_t header[kWavHeaderSize] = {};
  memcpy(header, "RIFF", 4);
  writeLittleEndian32(header, 4, dataSize + 36);
  memcpy(header + 8, "WAVEfmt ", 8);
  writeLittleEndian32(header, 16, 16);
  writeLittleEndian16(header, 20, 1);
  writeLittleEndian16(header, 22, 1);
  writeLittleEndian32(header, 24, kSampleRate);
  writeLittleEndian32(header, 28, kSampleRate * kWavSampleBytes);
  writeLittleEndian16(header, 32, kWavSampleBytes);
  writeLittleEndian16(header, 34, kWavSampleBytes * 8);
  memcpy(header + 36, "data", 4);
  writeLittleEndian32(header, 40, dataSize);
  return file.write(header, sizeof(header)) == sizeof(header);
}

uint32_t encodePcmSample(int32_t microphoneSample) {
#if AUDIO_BITS_PER_SAMPLE == 8
  int32_t rounded = ((microphoneSample >> 23) + 1) >> 1;
  if (rounded > 127) {
    rounded = 127;
  }
  return static_cast<uint8_t>(rounded + 128);
#else
#if AUDIO_BITS_PER_SAMPLE < 32
  microphoneSample >>= 32 - AUDIO_BITS_PER_SAMPLE;
#endif
  return static_cast<uint32_t>(microphoneSample);
#endif
}

int16_t decodeWavPcmSample(const uint8_t *sampleBytes) {
#if AUDIO_BITS_PER_SAMPLE == 8
  return static_cast<int16_t>((static_cast<int32_t>(sampleBytes[0]) - 128) * 256);
#else
  uint32_t pcm = 0;
  for (uint8_t byteIndex = 0; byteIndex < kWavSampleBytes; ++byteIndex) {
    pcm |= static_cast<uint32_t>(sampleBytes[byteIndex]) << (byteIndex * 8);
  }
#if AUDIO_BITS_PER_SAMPLE < 32
  if (pcm & (1UL << (AUDIO_BITS_PER_SAMPLE - 1))) {
    pcm |= 0xFFFFFFFFUL << AUDIO_BITS_PER_SAMPLE;
  }
#endif
  int32_t signedPcm = static_cast<int32_t>(pcm);
#if AUDIO_BITS_PER_SAMPLE < 16
  signedPcm *= 1L << (16 - AUDIO_BITS_PER_SAMPLE);
#elif AUDIO_BITS_PER_SAMPLE > 16
  signedPcm >>= AUDIO_BITS_PER_SAMPLE - 16;
#endif
  return static_cast<int16_t>(signedPcm);
#endif
}

bool writeMp3Sample(audio_tools::MP3EncoderShine &encoder,
                    uint8_t *pcmBuffer, size_t &bufferedBytes,
                    int16_t pcmSample) {
  pcmBuffer[bufferedBytes++] = static_cast<uint8_t>(pcmSample);
  pcmBuffer[bufferedBytes++] =
      static_cast<uint8_t>(static_cast<uint16_t>(pcmSample) >> 8);
  if (bufferedBytes == kMp3PcmBufferSize) {
    if (encoder.write(pcmBuffer, bufferedBytes) != bufferedBytes) {
      return false;
    }
    bufferedBytes = 0;
  }
  return true;
}

// Capacity-limited Print that never returns a short write (AudioTools loops on it).
class RamMp3Sink : public Print {
 public:
  using Print::write;

  void reset() {
    size_ = 0;
    overflow_ = false;
  }

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t *data, size_t length) override {
    const size_t room = kMp3RamBufferSize - size_;
    const size_t copied = length < room ? length : room;
    memcpy(buffer_ + size_, data, copied);
    size_ += copied;
    if (copied < length) {
      overflow_ = true;
    }
    return length;
  }

  const uint8_t *data() const { return buffer_; }
  size_t size() const { return size_; }
  bool overflow() const { return overflow_; }

 private:
  uint8_t buffer_[kMp3RamBufferSize];
  size_t size_ = 0;
  bool overflow_ = false;
};

// Linear interpolation from kSampleRate to kMp3SampleRate; emit() returns false to stop.
struct Mp3Resampler {
  uint32_t inputIndex = 0;
  uint64_t nextOutputPosition = 0;
  int16_t previousSample = 0;

  template <typename Emit>
  bool push(int16_t currentSample, Emit emit) {
    if (kSampleRate == kMp3SampleRate) {
      return emit(currentSample);
    }
    bool ok = true;
    if (inputIndex == 0) {
      ok = emit(currentSample);
      nextOutputPosition = kSampleRate;
    } else {
      const uint64_t segmentStartPosition =
          static_cast<uint64_t>(inputIndex - 1) * kMp3SampleRate;
      const uint64_t segmentEndPosition =
          static_cast<uint64_t>(inputIndex) * kMp3SampleRate;
      while (ok && nextOutputPosition <= segmentEndPosition) {
        const int64_t fraction =
            static_cast<int64_t>(nextOutputPosition - segmentStartPosition);
        const int64_t sampleDelta =
            static_cast<int32_t>(currentSample) - previousSample;
        ok = emit(static_cast<int16_t>(previousSample +
                                       sampleDelta * fraction / kMp3SampleRate));
        nextOutputPosition += kSampleRate;
      }
    }
    previousSample = currentSample;
    ++inputIndex;
    return ok;
  }
};

// Core 0 produces PCM into the ring, core 1 consumes it; head/tail are free-running
// counters, each written by exactly one core, so no lock is needed around them.
int16_t ramPcmRing[kPcmRingSamples];
volatile uint32_t ramRingHead = 0;
volatile uint32_t ramRingTail = 0;
volatile bool ramProducerDone = false;
volatile bool ramAbort = false;
volatile bool ramEncoderFailed = false;
volatile bool ramSyncReady = false;
semaphore_t ramEncodeStart;
semaphore_t ramEncodeDone;
uint8_t ramPcmStage[kMp3PcmBufferSize];
RamMp3Sink ramMp3Sink;

// Runs on core 1. Each encoder.write() is at most kMp3PcmBufferSize bytes, so it
// encodes at most one MP3 frame (~16 ms) before the ring tail is published again.
void encodeRamRecording() {
  audio_tools::MP3EncoderShine encoder;
  encoder.setBitrate(kMp3BitrateKbps);
  encoder.setAudioInfo(audio_tools::AudioInfo(kMp3SampleRate, 1, 16));
  encoder.setOutput(ramMp3Sink);

  Mp3Resampler resampler;
  size_t stagedBytes = 0;
  uint32_t tail = 0;
  bool encodingOk = encoder.begin();

  while (true) {
    if (!encodingOk) {
      ramEncoderFailed = true;
      break;
    }
    if (ramAbort) {
      break;
    }

    const bool producerFinished = ramProducerDone;
    __mem_fence_acquire();
    const uint32_t head = ramRingHead;
    __mem_fence_acquire();
    uint32_t pending = head - tail;
    if (pending == 0) {
      if (producerFinished) {
        break;
      }
      delay(1);
      continue;
    }
    if (pending > kRingConsumeSamples) {
      pending = kRingConsumeSamples;
    }

    for (uint32_t n = 0; n < pending && encodingOk; ++n) {
      const int16_t sample = ramPcmRing[tail++ & kPcmRingMask];
      encodingOk = resampler.push(sample, [&](int16_t outputSample) {
        return writeMp3Sample(encoder, ramPcmStage, stagedBytes, outputSample);
      });
    }
    __mem_fence_release();
    ramRingTail = tail;
    encodingOk = encodingOk && !ramMp3Sink.overflow();
  }

  if (encodingOk && !ramAbort && stagedBytes > 0) {
    encodingOk = encoder.write(ramPcmStage, stagedBytes) == stagedBytes;
  }
  encoder.end();
  if (!encodingOk || ramMp3Sink.overflow()) {
    ramEncoderFailed = true;
  }
}

void setup1() {
  while (!ramSyncReady) {
    delay(1);
  }
  __mem_fence_acquire();
}

void loop1() {
  sem_acquire_blocking(&ramEncodeStart);
  encodeRamRecording();
  sem_release(&ramEncodeDone);
}

bool listFiles(const char *path, uint8_t depth) {
  bool found = false;
  fs::Dir directory = LittleFS.openDir(path);
  while (directory.next()) {
    found = true;
    for (uint8_t i = 0; i < depth; ++i) {
      serialPortUsed.print(F("  "));
    }
    serialPortUsed.print(directory.fileName());
    if (directory.isDirectory()) {
      serialPortUsed.println('/');
      String childPath(path);
      if (!childPath.endsWith("/")) {
        childPath += '/';
      }
      childPath += directory.fileName();
      listFiles(childPath.c_str(), depth + 1);
    } else {
      serialPortUsed.print(F("  "));
      serialPortUsed.print(directory.fileSize());
      serialPortUsed.println(F(" bytes"));
    }
  }
  return found;
}

String readLine() {
  String line;
  while (true) {
    while (serialPortUsed.available()) {
      const char character = static_cast<char>(serialPortUsed.read());
      if (character == '\r' || character == '\n') {
        if (line.length() > 0) {
          line.trim();
          return line;
        }
      } else if (character >= ' ') {
        line += character;
      }
    }
    delay(1);
  }
}

void showMenu() {
  serialPortUsed.println();
  serialPortUsed.println(F("LittleFS content:"));
  if (!fileSystemReady) {
    serialPortUsed.println(F("  unavailable"));
  } else if (!listFiles("/", 0)) {
    serialPortUsed.println(F("  (empty)"));
  }
  serialPortUsed.println();
  serialPortUsed.println(F("What to do?"));
  serialPortUsed.println(F("  1 - Record a 5 second voice message"));
  serialPortUsed.println(F("  2 - Show recording size and send by XModem"));
  serialPortUsed.println(
      F("  3 - Record to RAM (MP3 encoded live on core 1), save later and send by XModem"));
  serialPortUsed.println(F("  4 - Record a 5 second voice message, volume compressed with Maximilian"));
  serialPortUsed.print(F("> "));
}

bool encodeVoiceToMp3() {
  fs::File wav = LittleFS.open(kRecordingPath, "r");
  if (!wav || wav.size() < kWavHeaderSize + kSampleCount * kWavSampleBytes ||
      !wav.seek(kWavHeaderSize)) {
    if (wav) {
      wav.close();
    }
    serialPortUsed.println(F("Could not read the WAV recording for MP3 encoding."));
    return false;
  }

  fs::File mp3 = LittleFS.open(kMp3RecordingPath, "w");
  if (!mp3) {
    wav.close();
    serialPortUsed.println(F("Could not create the MP3 recording file."));
    return false;
  }

  audio_tools::MP3EncoderShine encoder;
  encoder.setBitrate(kMp3BitrateKbps);
  encoder.setAudioInfo(audio_tools::AudioInfo(kMp3SampleRate, 1, 16));
  encoder.setOutput(mp3);

  const uint32_t encodingStart = millis();
  bool encodingOk = encoder.begin();
  uint32_t remainingBytes = kSampleCount * kWavSampleBytes;
  uint32_t inputSampleIndex = 0;
  uint64_t nextOutputPosition = 0;
  int16_t previousPcmSample = 0;
  uint8_t wavBuffer[512];
  uint8_t pcmBuffer[kMp3PcmBufferSize];
  size_t bufferedPcmBytes = 0;

  while (encodingOk && remainingBytes > 0) {
    const size_t chunkBytes = remainingBytes < sizeof(wavBuffer)
                                  ? remainingBytes
                                  : sizeof(wavBuffer);
    const size_t bytesRead = wav.read(wavBuffer, chunkBytes);
    if (bytesRead != chunkBytes) {
      encodingOk = false;
      break;
    }

    const size_t sampleCount = bytesRead / kWavSampleBytes;
    for (size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
      const int16_t currentPcmSample = decodeWavPcmSample(
          wavBuffer + sampleIndex * kWavSampleBytes);
      if (inputSampleIndex == 0) {
        encodingOk = writeMp3Sample(encoder, pcmBuffer, bufferedPcmBytes,
                                    currentPcmSample);
        nextOutputPosition = kSampleRate;
      } else {
        const uint64_t segmentStartPosition =
            static_cast<uint64_t>(inputSampleIndex - 1) * kMp3SampleRate;
        const uint64_t segmentEndPosition =
            static_cast<uint64_t>(inputSampleIndex) * kMp3SampleRate;
        while (encodingOk && nextOutputPosition <= segmentEndPosition) {
          const int64_t fraction =
              static_cast<int64_t>(nextOutputPosition - segmentStartPosition);
          const int64_t sampleDelta =
              static_cast<int32_t>(currentPcmSample) - previousPcmSample;
          const int16_t interpolatedSample = static_cast<int16_t>(
              previousPcmSample +
              sampleDelta * fraction / kMp3SampleRate);
          encodingOk = writeMp3Sample(encoder, pcmBuffer, bufferedPcmBytes,
                                      interpolatedSample);
          nextOutputPosition += kSampleRate;
        }
      }
      previousPcmSample = currentPcmSample;
      ++inputSampleIndex;
      if (!encodingOk) {
        break;
      }
    }

    remainingBytes -= bytesRead;
  }

  if (encodingOk && bufferedPcmBytes > 0) {
    encodingOk = encoder.write(pcmBuffer, bufferedPcmBytes) == bufferedPcmBytes;
  }

  encoder.end();
  mp3.flush();
  const uint32_t encodingTimeMs = millis() - encodingStart;
  const size_t mp3Size = mp3.size();
  wav.close();
  mp3.close();

  if (!encodingOk || mp3Size == 0) {
    LittleFS.remove(kMp3RecordingPath);
    serialPortUsed.println(F("MP3 encoding failed; incomplete file removed."));
    return false;
  }

  serialPortUsed.print(F("Saved "));
  serialPortUsed.print(kMp3RecordingPath);
  serialPortUsed.print(F(" ("));
  serialPortUsed.print(mp3Size);
  serialPortUsed.print(F(" bytes). Encoding took "));
  serialPortUsed.print(encodingTimeMs);
  serialPortUsed.println(F(" ms."));
  return true;
}

bool configureMicrophone() {
  return microphone.setBCLK(kMicBclkPin) && microphone.setDATA(kMicDataPin) &&
         microphone.setBitsPerSample(32) &&
         microphone.setFrequency(kSampleRate) && microphone.setBuffers(6, 512);
}

void recordVoice(bool compress) {
  if (!fileSystemReady) {
    serialPortUsed.println(F("LittleFS is not available."));
    return;
  }

  FSInfo info;
  if (!LittleFS.info(info)) {
    serialPortUsed.println(F("Could not read LittleFS capacity."));
    return;
  }

  const uint32_t dataSize = kSampleCount * kWavSampleBytes;
  const uint32_t requiredBytes =
      dataSize + kWavHeaderSize + kMp3EstimatedSize + 4096;
  uint32_t freeBytes = info.totalBytes > info.usedBytes
                           ? info.totalBytes - info.usedBytes
                           : 0;
  fs::File previousRecording = LittleFS.open(kRecordingPath, "r");
  if (previousRecording) {
    freeBytes += previousRecording.size();
    previousRecording.close();
  }
  fs::File previousMp3 = LittleFS.open(kMp3RecordingPath, "r");
  if (previousMp3) {
    freeBytes += previousMp3.size();
    previousMp3.close();
  }
  if (freeBytes < requiredBytes) {
    serialPortUsed.println(F("Not enough LittleFS space for a 5 second recording."));
    return;
  }

  if (!configureMicrophone()) {
    serialPortUsed.println(F("Could not configure I2S microphone."));
    return;
  }

  fs::File recording = LittleFS.open(kRecordingPath, "w");
  if (!recording) {
    serialPortUsed.println(F("Could not create the recording file."));
    return;
  }
  if (!writeWavHeader(recording, dataSize)) {
    recording.close();
    LittleFS.remove(kRecordingPath);
    serialPortUsed.println(F("Could not write the WAV header."));
    return;
  }
  if (!microphone.begin()) {
    recording.close();
    LittleFS.remove(kRecordingPath);
    serialPortUsed.println(F("Could not start I2S microphone."));
    return;
  }

  if (compress) {
    voiceCompressorBegin(kSampleRate);
  }
  serialPortUsed.println(F("Recording for 5 seconds..."));
  uint8_t pcmBuffer[512];
  size_t bufferedBytes = 0;
  bool recordingOk = true;
  for (uint32_t sampleIndex = 0; sampleIndex < kSampleCount; ++sampleIndex) {
    int32_t leftSample;
    int32_t rightSample;
    if (!microphone.read32(&leftSample, &rightSample)) {
      recordingOk = false;
      break;
    }
    int32_t microphoneSample = kMicRightChannel ? rightSample : leftSample;
    if (compress) {
      microphoneSample = voiceCompressorProcess(microphoneSample);
    }
    if (bufferedBytes + kWavSampleBytes > sizeof(pcmBuffer)) {
      if (recording.write(pcmBuffer, bufferedBytes) != bufferedBytes) {
        recordingOk = false;
        break;
      }
      bufferedBytes = 0;
    }
    const uint32_t pcm = encodePcmSample(microphoneSample);
    for (uint8_t byteIndex = 0; byteIndex < kWavSampleBytes; ++byteIndex) {
      pcmBuffer[bufferedBytes++] = static_cast<uint8_t>(pcm >> (byteIndex * 8));
    }
    if (bufferedBytes == sizeof(pcmBuffer)) {
      if (recording.write(pcmBuffer, bufferedBytes) != bufferedBytes) {
        recordingOk = false;
        break;
      }
      bufferedBytes = 0;
    }
  }

  if (recordingOk && bufferedBytes > 0) {
    recordingOk = recording.write(pcmBuffer, bufferedBytes) == bufferedBytes;
  }
  recording.flush();
  recording.close();
  microphone.end();

  if (!recordingOk) {
    LittleFS.remove(kRecordingPath);
    serialPortUsed.println(F("Recording failed; incomplete file removed."));
    return;
  }

  fs::File savedRecording = LittleFS.open(kRecordingPath, "r");
  serialPortUsed.print(F("Saved "));
  serialPortUsed.print(kRecordingPath);
  serialPortUsed.print(F(" ("));
  serialPortUsed.print(savedRecording ? savedRecording.size() : 0);
  serialPortUsed.println(F(" bytes)."));
  savedRecording.close();
  encodeVoiceToMp3();
}

void sendRecordingByXModem(const char *recordingPath) {
  fs::File recording = LittleFS.open(recordingPath, "r");
  if (!recording) {
    serialPortUsed.println(F("Could not open the selected recording."));
    return;
  }
  const size_t fileSize = recording.size();
  recording.close();

  serialPortUsed.print(F("Sending "));
  serialPortUsed.print(recordingPath);
  serialPortUsed.print(F(" ("));
  serialPortUsed.print(fileSize);
  serialPortUsed.println(F(" bytes)."));
  serialPortUsed.println(F("Start an XMODEM/CRC receive on this serial connection now."));
  serialPortUsed.flush();

  const bool sent = myXModem.sendFile(recordingPath);
  serialPortUsed.println(sent ? F("XModem transfer complete.")
                      : F("XModem transfer failed."));
}

void recordToRamSaveLater() {
  if (!fileSystemReady) {
    serialPortUsed.println(F("LittleFS is not available."));
    return;
  }

  FSInfo info;
  if (!LittleFS.info(info)) {
    serialPortUsed.println(F("Could not read LittleFS capacity."));
    return;
  }
  uint32_t freeBytes = info.totalBytes > info.usedBytes
                           ? info.totalBytes - info.usedBytes
                           : 0;
  fs::File previousRecording = LittleFS.open(kRamMp3RecordingPath, "r");
  if (previousRecording) {
    freeBytes += previousRecording.size();
    previousRecording.close();
  }
  if (freeBytes < kMp3RamBufferSize + 4096) {
    serialPortUsed.println(F("Not enough LittleFS space for the MP3 recording."));
    return;
  }

  if (!configureMicrophone()) {
    serialPortUsed.println(F("Could not configure I2S microphone."));
    return;
  }

  // Core 1 is idle (blocked on ramEncodeStart), so it is safe to reset shared state here.
  ramMp3Sink.reset();
  ramRingHead = 0;
  ramRingTail = 0;
  ramProducerDone = false;
  ramAbort = false;
  ramEncoderFailed = false;
  while (sem_try_acquire(&ramEncodeDone)) {
  }
  sem_release(&ramEncodeStart);

  const __FlashStringHelper *failure = nullptr;
  uint32_t head = 0;
  uint32_t ringPeak = 0;
  if (!microphone.begin()) {
    failure = F("Could not start I2S microphone.");
  } else {
    serialPortUsed.println(F("Recording for 5 seconds into RAM..."));
    for (uint32_t sampleIndex = 0; sampleIndex < kSampleCount; ++sampleIndex) {
      int32_t leftSample;
      int32_t rightSample;
      if (!microphone.read32(&leftSample, &rightSample)) {
        failure = F("I2S read failed.");
        break;
      }
      const uint32_t queued = head - ramRingTail;
      if (queued >= kPcmRingSamples) {
        failure = F("PCM ring overrun: the MP3 encoder cannot keep up.");
        break;
      }
      if (queued > ringPeak) {
        ringPeak = queued;
      }
      const int32_t microphoneSample = kMicRightChannel ? rightSample : leftSample;
      ramPcmRing[head++ & kPcmRingMask] = static_cast<int16_t>(microphoneSample >> 16);
      if ((head & (kRingPublishSamples - 1)) == 0) {
        __mem_fence_release();
        ramRingHead = head;
        if (ramEncoderFailed) {
          failure = F("MP3 encoder failed while recording.");
          break;
        }
      }
    }
    microphone.end();
  }
  const uint32_t recordingEndMs = millis();

  __mem_fence_release();
  ramRingHead = head;
  if (failure) {
    ramAbort = true;
  }
  __mem_fence_release();
  ramProducerDone = true;
  sem_acquire_blocking(&ramEncodeDone);
  const uint32_t encoderLagMs = millis() - recordingEndMs;

  if (!failure && (ramEncoderFailed || ramMp3Sink.overflow() ||
                   ramMp3Sink.size() == 0)) {
    failure = F("MP3 encoding failed or overflowed the RAM buffer.");
  }
  if (failure) {
    serialPortUsed.print(F("Recording failed: "));
    serialPortUsed.println(failure);
    return;
  }

  const size_t mp3Size = ramMp3Sink.size();
  fs::File mp3 = LittleFS.open(kRamMp3RecordingPath, "w");
  if (!mp3) {
    serialPortUsed.println(F("Could not create the MP3 recording file."));
    return;
  }
  const bool written = mp3.write(ramMp3Sink.data(), mp3Size) == mp3Size;
  mp3.flush();
  mp3.close();
  if (!written) {
    LittleFS.remove(kRamMp3RecordingPath);
    serialPortUsed.println(F("Could not save the MP3 recording; incomplete file removed."));
    return;
  }

  serialPortUsed.print(F("Saved "));
  serialPortUsed.print(kRamMp3RecordingPath);
  serialPortUsed.print(F(" ("));
  serialPortUsed.print(mp3Size);
  serialPortUsed.print(F(" bytes). Encoder finished "));
  serialPortUsed.print(encoderLagMs);
  serialPortUsed.print(F(" ms after the last sample; PCM ring peak "));
  serialPortUsed.print(ringPeak);
  serialPortUsed.print(F("/"));
  serialPortUsed.print(kPcmRingSamples);
  serialPortUsed.println(F(" samples."));

  if (!xmodemReady) {
    serialPortUsed.println(F("XModem is not available; the file stays on LittleFS."));
    return;
  }
  sendRecordingByXModem(kRamMp3RecordingPath);
}

void showSizeAndSend() {
  if (!fileSystemReady) {
    serialPortUsed.println(F("LittleFS is not available."));
    return;
  }
  if (!xmodemReady) {
    serialPortUsed.println(F("XModem is not available."));
    return;
  }

  const bool wavAvailable = LittleFS.exists(kRecordingPath);
  const bool mp3Available = LittleFS.exists(kMp3RecordingPath);
  if (!wavAvailable && !mp3Available) {
    serialPortUsed.println(F("No voice.wav or voice.mp3 recording is available."));
    return;
  }

  serialPortUsed.println(F("Which recording should be sent?"));
  serialPortUsed.print(F("  1 - voice.wav"));
  if (!wavAvailable) {
    serialPortUsed.print(F(" (not available)"));
  }
  serialPortUsed.println();
  serialPortUsed.print(F("  2 - voice.mp3"));
  if (!mp3Available) {
    serialPortUsed.print(F(" (not available)"));
  }
  serialPortUsed.println();
  serialPortUsed.print(F("> "));

  const String choice = readLine();
  const char *recordingPath;
  if (choice == "1") {
    recordingPath = kRecordingPath;
  } else if (choice == "2") {
    recordingPath = kMp3RecordingPath;
  } else {
    serialPortUsed.println(F("Choose 1 or 2."));
    return;
  }

  sendRecordingByXModem(recordingPath);
}

// Binary search for the largest block the allocator can hand out right now.
template <typename Allocate, typename Release>
size_t largestAllocation(Allocate allocate, Release release) {
  size_t low = 0;
  size_t high =
      static_cast<size_t>(rp2040.getTotalHeap() + rp2040.getTotalPSRAMHeap());
  while (low < high) {
    const size_t middle = low + (high - low + 1) / 2;
    void *block = allocate(middle);
    if (block) {
      // Touch both ends so the compiler cannot elide the allocation.
      volatile uint8_t *bytes = static_cast<volatile uint8_t *>(block);
      bytes[0] = 0xA5;
      bytes[middle - 1] = 0xA5;
      release(block);
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return low;
}

void reportAvailableMemory() {
  const struct mallinfo heapInfo = mallinfo();
  const int totalHeap = rp2040.getTotalHeap();
  const size_t largestMalloc = largestAllocation(
      [](size_t size) -> void * { return malloc(size); },
      [](void *block) { free(block); });

  serialPortUsed.println(F("Memory available at boot:"));
  serialPortUsed.printf("  rp2040 API: heap %d, used %d, free %d bytes\r\n", totalHeap,
                        rp2040.getUsedHeap(), rp2040.getFreeHeap());
  serialPortUsed.printf(
      "  mallinfo(): arena %d, in use %d, free chunks %d, never claimed %d bytes\r\n",
      heapInfo.arena, heapInfo.uordblks, heapInfo.fordblks,
      totalHeap - heapInfo.arena);
  serialPortUsed.printf("  largest malloc() block: %u bytes\r\n",
                        static_cast<unsigned>(largestMalloc));
}

void setup() {
  serialPortUsed.begin(115200);
  const uint32_t serialWaitStart = millis();
  while (!serialPortUsed && millis() - serialWaitStart < 2000) {
    delay(10);
  }

  serialPortUsed.println();
  serialPortUsed.println(F("Past Recorder"));
  serialPortUsed.println(F("Hello! Ready to record a voice message."));

  fileSystemReady = LittleFS.begin();
  serialPortUsed.println(fileSystemReady ? F("LittleFS mounted.")
                                 : F("LittleFS mount failed."));

  myXModem.onXmodemUpdate(onXmodemUpdate);
  xmodemReady = myXModem.begin(&serialPortUsed, XModem::CRC_XMODEM, LittleFS);
  if (!xmodemReady) {
    serialPortUsed.println(F("XModem initialization failed."));
  }

  sem_init(&ramEncodeStart, 0, 1);
  sem_init(&ramEncodeDone, 0, 1);
  __mem_fence_release();
  ramSyncReady = true;

  reportAvailableMemory();
}

void loop() {
  showMenu();
  const String choice = readLine();
  if (choice == "1") {
    recordVoice(false);
  } else if (choice == "2") {
    showSizeAndSend();
  } else if (choice == "3") {
    recordToRamSaveLater();
  } else if (choice == "4") {
    recordVoice(true);
  } else {
    serialPortUsed.println(F("Choose 1, 2, 3 or 4."));
  }
}