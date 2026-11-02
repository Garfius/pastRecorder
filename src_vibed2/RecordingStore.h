#pragma once

#include <Arduino.h>
#include <FS.h>

#include "AudioBuffers.h"

enum class StorageKind : uint8_t { Sd, LittleFs };

class RecordingStore {
 public:
  void begin();
  void end();
  void setService(bool (*service)()) { service_ = service; }
  fs::FS &filesystem(StorageKind kind);
  const char *name(StorageKind kind) const;
  const char *status(StorageKind kind) const;
  bool mounted(StorageKind kind) const;
  bool space(StorageKind kind, uint64_t required, FSInfo *info = nullptr);
  bool available();
  bool save(const Mp3FrameRing &ring);
  void report(Print &out);
  bool list(StorageKind kind, Print &out);
  bool erase(StorageKind kind, Print &out);
  bool copyToSd(const char *folder, Print &out);
  bool formatLittleFs();
  const char *lastPath() const { return lastPath_; }
  StorageKind lastKind() const { return lastKind_; }

 private:
  bool saveTo(StorageKind kind, const Mp3FrameRing &ring);
  bool walk(StorageKind kind, const char *path, unsigned depth, Print &out,
            bool eraseFiles, const char *copyRoot = nullptr);
  bool copyFile(const char *source, const char *destination);
  bool removePartial(fs::FS &fs, const char *path);
  bool sdReady_ = false;
  bool flashReady_ = false;
  const char *sdStatus_ = "not mounted";
  const char *flashStatus_ = "not mounted";
  uint32_t nextId_ = 1;
  StorageKind lastKind_ = StorageKind::LittleFs;
  char lastPath_[32] = {};
  bool (*service_)() = nullptr;
};
