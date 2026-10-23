#include "RecordingStore.h"

#include "AudioConfig.h"

bool RecordingStore::begin() {
  ready_ = LittleFS.begin();
  return ready_;
}

bool RecordingStore::isReady() const {
  return ready_;
}

RecordingCapacity RecordingStore::checkRecordingCapacity() const {
  FSInfo info;
  if (!LittleFS.info(info)) {
    return RecordingCapacity::CannotRead;
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

  return freeBytes >= requiredBytes ? RecordingCapacity::Available
                                   : RecordingCapacity::Insufficient;
}

bool RecordingStore::exists(const char *path) const {
  return LittleFS.exists(path);
}

fs::File RecordingStore::open(const char *path, const char *mode) const {
  return LittleFS.open(path, mode);
}

bool RecordingStore::remove(const char *path) const {
  return LittleFS.remove(path);
}

bool RecordingStore::listFiles(Print &output) const {
  return listFiles(output, "/", 0);
}

bool RecordingStore::listFiles(Print &output, const char *path,
                               uint8_t depth) const {
  bool found = false;
  fs::Dir directory = LittleFS.openDir(path);
  while (directory.next()) {
    found = true;
    for (uint8_t index = 0; index < depth; ++index) {
      output.print(F("  "));
    }
    output.print(directory.fileName());
    if (directory.isDirectory()) {
      output.println('/');
      String childPath(path);
      if (!childPath.endsWith("/")) {
        childPath += '/';
      }
      childPath += directory.fileName();
      listFiles(output, childPath.c_str(), depth + 1);
    } else {
      output.print(F("  "));
      output.print(directory.fileSize());
      output.println(F(" bytes"));
    }
  }
  return found;
}