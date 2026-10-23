#pragma once

#include <Arduino.h>
#include <LittleFS.h>

enum class RecordingCapacity {
  Available,
  CannotRead,
  Insufficient
};

class RecordingStore {
 public:
  bool begin();
  bool isReady() const;
  RecordingCapacity checkRecordingCapacity() const;
  bool exists(const char *path) const;
  fs::File open(const char *path, const char *mode) const;
  bool remove(const char *path) const;
  bool listFiles(Print &output) const;

 private:
  bool listFiles(Print &output, const char *path, uint8_t depth) const;

  bool ready_ = false;
};