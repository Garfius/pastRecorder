#include "AudioServices.h"

//#include <AudioTools.h>
#include <AudioTools/AudioCodecs/CodecMP3Shine.h>

#include "AudioConfig.h"
#include "WavFormat.h"

namespace {

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

}

VoiceRecorder::VoiceRecorder(Print &output)
    : output_(output), microphone_(INPUT) {}

bool VoiceRecorder::record(RecordingStore &store) {
  if (!store.isReady()) {
    output_.println(F("LittleFS is not available."));
    return false;
  }

  const RecordingCapacity capacity = store.checkRecordingCapacity();
  if (capacity == RecordingCapacity::CannotRead) {
    output_.println(F("Could not read LittleFS capacity."));
    return false;
  }
  if (capacity == RecordingCapacity::Insufficient) {
    output_.println(F("Not enough LittleFS space for a 5 second recording."));
    return false;
  }

  if (!microphone_.setBCLK(kMicBclkPin) || !microphone_.setDATA(kMicDataPin) ||
      !microphone_.setBitsPerSample(32) ||
      !microphone_.setFrequency(kSampleRate) ||
      !microphone_.setBuffers(6, 512)) {
    output_.println(F("Could not configure I2S microphone."));
    return false;
  }

  const uint32_t dataSize = kSampleCount * kWavSampleBytes;
  fs::File recording = store.open(kRecordingPath, "w");
  if (!recording) {
    output_.println(F("Could not create the recording file."));
    return false;
  }
  if (!WavFormat::writeHeader(recording, dataSize)) {
    recording.close();
    store.remove(kRecordingPath);
    output_.println(F("Could not write the WAV header."));
    return false;
  }
  if (!microphone_.begin()) {
    recording.close();
    store.remove(kRecordingPath);
    output_.println(F("Could not start I2S microphone."));
    return false;
  }

  output_.println(F("Recording for 5 seconds..."));
  uint8_t pcmBuffer[512];
  size_t bufferedBytes = 0;
  bool recordingOk = true;
  for (uint32_t sampleIndex = 0; sampleIndex < kSampleCount; ++sampleIndex) {
    int32_t leftSample;
    int32_t rightSample;
    if (!microphone_.read32(&leftSample, &rightSample)) {
      recordingOk = false;
      break;
    }
    const int32_t microphoneSample = kMicRightChannel ? rightSample : leftSample;
    if (bufferedBytes + kWavSampleBytes > sizeof(pcmBuffer)) {
      if (recording.write(pcmBuffer, bufferedBytes) != bufferedBytes) {
        recordingOk = false;
        break;
      }
      bufferedBytes = 0;
    }
    const uint32_t pcm = WavFormat::encodeSample(microphoneSample);
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
  microphone_.end();

  if (!recordingOk) {
    store.remove(kRecordingPath);
    output_.println(F("Recording failed; incomplete file removed."));
    return false;
  }

  fs::File savedRecording = store.open(kRecordingPath, "r");
  output_.print(F("Saved "));
  output_.print(kRecordingPath);
  output_.print(F(" ("));
  output_.print(savedRecording ? savedRecording.size() : 0);
  output_.println(F(" bytes)."));
  savedRecording.close();
  return true;
}

Mp3Transcoder::Mp3Transcoder(Print &output) : output_(output) {}

bool Mp3Transcoder::encode(RecordingStore &store) {
  fs::File wav = store.open(kRecordingPath, "r");
  if (!wav || wav.size() < kWavHeaderSize + kSampleCount * kWavSampleBytes ||
      !wav.seek(kWavHeaderSize)) {
    if (wav) {
      wav.close();
    }
    output_.println(F("Could not read the WAV recording for MP3 encoding."));
    return false;
  }

  fs::File mp3 = store.open(kMp3RecordingPath, "w");
  if (!mp3) {
    wav.close();
    output_.println(F("Could not create the MP3 recording file."));
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
      const int16_t currentPcmSample = WavFormat::decodeSample(
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
    store.remove(kMp3RecordingPath);
    output_.println(F("MP3 encoding failed; incomplete file removed."));
    return false;
  }

  output_.print(F("Saved "));
  output_.print(kMp3RecordingPath);
  output_.print(F(" ("));
  output_.print(mp3Size);
  output_.print(F(" bytes). Encoding took "));
  output_.print(encodingTimeMs);
  output_.println(F(" ms."));
  return true;
}