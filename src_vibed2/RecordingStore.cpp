#include "RecordingStore.h"

#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

namespace {
bool joinPath(char *out, size_t capacity, const char *root, const char *leaf) {
  const int count = snprintf(out, capacity, "%s%s%s", root,
                             strcmp(root, "/") == 0 ? "" : "/", leaf);
  return count >= 0 && static_cast<size_t>(count) < capacity;
}
}

void RecordingStore::begin() {
  // Called only outside capture/transfers; re-probe even a swapped SD card.
  SD.end();
  CFG_SD_SPI.end();
  const bool pins = CFG_SD_SPI.setSCK(CFG_SD_SCK_PIN) &&
                    CFG_SD_SPI.setRX(CFG_SD_MISO_PIN) &&
                    CFG_SD_SPI.setTX(CFG_SD_MOSI_PIN);
  sdReady_ = pins && SD.begin(CFG_SD_CS_PIN, CFG_SD_SPI_HZ, CFG_SD_SPI);
  sdStatus_ = sdReady_ ? "ready" : "pin configuration/mount failed";
  if (!flashReady_) {
    // Never auto-format potentially recoverable recordings.
    LittleFS.setConfig(LittleFSConfig(false));
    flashReady_ = LittleFS.begin();
    flashStatus_ = flashReady_ ? "ready" : "mount failed (format requires menu)";
  }
}

void RecordingStore::end() {
  SD.end();
  LittleFS.end();
  CFG_SD_SPI.end();
  sdReady_ = flashReady_ = false;
}

fs::FS &RecordingStore::filesystem(StorageKind kind) {
  // SD.h's Arduino-Pico adapter is backed by the same SDFS instance.
  return kind == StorageKind::Sd ? SDFS : LittleFS;
}

const char *RecordingStore::name(StorageKind kind) const {
  return kind == StorageKind::Sd ? "SD" : "LittleFS";
}

const char *RecordingStore::status(StorageKind kind) const {
  return kind == StorageKind::Sd ? sdStatus_ : flashStatus_;
}

bool RecordingStore::mounted(StorageKind kind) const {
  return kind == StorageKind::Sd ? sdReady_ : flashReady_;
}

bool RecordingStore::space(StorageKind kind, uint64_t required, FSInfo *result) {
  FSInfo info = {};
  if (!mounted(kind)) {
    return false;
  }
  const char *message = "ready";
  bool ok = filesystem(kind).info(info) && info.usedBytes <= info.totalBytes;
  if (!ok) {
    message = "capacity query failed";
  } else if (info.totalBytes - info.usedBytes <
             required + CFG_STORAGE_MARGIN_BYTES) {
    message = "full / insufficient free space";
    ok = false;
  }
  if (kind == StorageKind::Sd) {
    sdStatus_ = message;
  } else {
    flashStatus_ = message;
  }
  if (result) {
    *result = info;
  }
  return ok;
}

bool RecordingStore::available() {
  const bool sd = space(StorageKind::Sd, kMp3FrameBytes);
  const bool flash = space(StorageKind::LittleFs, kMp3FrameBytes);
  return sd || flash;
}

bool RecordingStore::removePartial(fs::FS &fs, const char *path) {
  return !fs.exists(path) || fs.remove(path);
}

bool RecordingStore::saveTo(StorageKind kind, const Mp3FrameRing &ring) {
  if (!space(kind, ring.size())) {
    return false;
  }
  fs::FS &fs = filesystem(kind);
  const char *&message = kind == StorageKind::Sd ? sdStatus_ : flashStatus_;
  char finalPath[32];
  char temporary[32];
  do {
    if (nextId_ == UINT32_MAX) {
      message = "recording filename counter exhausted";
      return false;
    }
    snprintf(finalPath, sizeof(finalPath), "/REC%08lu.mp3",
             static_cast<unsigned long>(nextId_));
    snprintf(temporary, sizeof(temporary), "/REC%08lu.tmp",
             static_cast<unsigned long>(nextId_++));
  } while (fs.exists(finalPath) || fs.exists(temporary));

  fs::File file = fs.open(temporary, "w");
  if (!file) {
    message = "create failed";
    return false;
  }
  bool ok = true;
  uint32_t expectedCrc = 0xFFFFFFFFu;
  for (size_t offset = 0; offset < ring.size() && ok;) {
    size_t count;
    const uint8_t *bytes = ring.segment(offset, count);
    if (count > 512) {
      count = 512;
    }
    ok = file.write(bytes, count) == count;
    expectedCrc = crc32Update(expectedCrc, bytes, count);
    offset += count;
    if (service_) {
      service_();
    }
  }
  file.flush();
  file.close();
  file = fs.open(temporary, "r");
  ok = ok && file && file.size() == ring.size();
  uint32_t actualCrc = 0xFFFFFFFFu;
  uint8_t buffer[512];
  for (size_t offset = 0; offset < ring.size() && ok;) {
    size_t count = ring.size() - offset;
    if (count > sizeof(buffer)) {
      count = sizeof(buffer);
    }
    ok = file.read(buffer, count) == static_cast<int>(count);
    if (ok) {
      actualCrc = crc32Update(actualCrc, buffer, count);
    }
    offset += count;
    if (service_) {
      service_();
    }
  }
  file.close();
  if (!ok || actualCrc != expectedCrc) {
    message = removePartial(fs, temporary) ?
                  "write/readback verification failed" :
                  "write failed AND partial-file removal failed";
    return false;
  }
  if (!fs.rename(temporary, finalPath)) {
    message = removePartial(fs, temporary) ?
                  "final rename failed" :
                  "rename failed AND partial-file removal failed";
    return false;
  }
  snprintf(lastPath_, sizeof(lastPath_), "%s", finalPath);
  lastKind_ = kind;
  message = "last save verified";
  return true;
}

bool RecordingStore::save(const Mp3FrameRing &ring) {
  if (!ring.size()) {
    return false;
  }
  begin(); // Retry an absent/reinserted SD card, then fall back without data loss.
  return saveTo(StorageKind::Sd, ring) ||
         saveTo(StorageKind::LittleFs, ring);
}

void RecordingStore::report(Print &out) {
  for (StorageKind kind : {StorageKind::Sd, StorageKind::LittleFs}) {
    const char *previous = status(kind);
    FSInfo info = {};
    const bool usable = space(kind, kMp3FrameBytes, &info);
    const uint64_t freeBytes = info.totalBytes >= info.usedBytes ?
                                   info.totalBytes - info.usedBytes : 0;
    out.printf("%s: %s; %s; total=%llu used=%llu free=%llu bytes\r\n",
               name(kind), previous, usable ? "writable capacity" : status(kind),
               static_cast<unsigned long long>(info.totalBytes),
               static_cast<unsigned long long>(info.usedBytes),
               static_cast<unsigned long long>(freeBytes));
  }
}

bool RecordingStore::list(StorageKind kind, Print &out) {
  if (!mounted(kind)) {
    out.println("ERROR: selected filesystem is not mounted.");
    return false;
  }
  return walk(kind, "/", 0, out, false);
}

bool RecordingStore::erase(StorageKind kind, Print &out) {
  if (!mounted(kind)) {
    out.println("ERROR: selected filesystem is not mounted.");
    return false;
  }
  return walk(kind, "/", 0, out, true);
}

bool RecordingStore::walk(StorageKind kind, const char *path, unsigned depth,
                          Print &out, bool eraseFiles, const char *copyRoot) {
  if (depth > CFG_STORAGE_MAX_DEPTH) {
    out.printf("ERROR: directory depth limit at %s\r\n", path);
    return false;
  }
  fs::FS &fs = filesystem(kind);
  fs::File check = fs.open(path, "r");
  if (!check || !check.isDirectory()) {
    out.printf("ERROR: cannot open directory %s\r\n", path);
    return false;
  }
  check.close();
  fs::Dir dir = fs.openDir(path);
  bool ok = true;
  while (dir.next()) {
    if (service_ && !service_()) {
      out.println("ERROR: operation cancelled for pending power shutdown.");
      return false;
    }
    char source[CFG_STORAGE_PATH_BYTES];
    const String entry = dir.fileName();
    const char *leaf = strrchr(entry.c_str(), '/');
    leaf = leaf ? leaf + 1 : entry.c_str();
    if (!joinPath(source, sizeof(source), path, leaf)) {
      out.println("ERROR: path too long.");
      return false;
    }
    char destination[CFG_STORAGE_PATH_BYTES];
    if (copyRoot) {
      const int count = snprintf(destination, sizeof(destination), "%s%s",
                                 copyRoot, source);
      if (count < 0 || static_cast<size_t>(count) >= sizeof(destination)) {
        out.println("ERROR: destination path too long.");
        return false;
      }
    }
    if (dir.isDirectory()) {
      if (copyRoot && !SDFS.mkdir(destination)) {
        out.printf("ERROR: cannot create %s\r\n", destination);
        ok = false;
        continue;
      }
      const bool walked = walk(kind, source, depth + 1, out, eraseFiles, copyRoot);
      ok = walked && ok;
      if (eraseFiles && walked && !fs.rmdir(source)) {
        out.printf("ERROR: cannot remove directory %s\r\n", source);
        ok = false;
      }
    } else if (eraseFiles) {
      if (!fs.remove(source)) {
        out.printf("ERROR: cannot erase %s\r\n", source);
        ok = false;
      }
    } else if (copyRoot) {
      if (!copyFile(source, destination)) {
        out.printf("ERROR: copy failed at %s\r\n", source);
        ok = false;
      }
    } else {
      out.printf("%s  %lu bytes\r\n", source,
                 static_cast<unsigned long>(dir.fileSize()));
    }
  }
  return ok;
}

bool RecordingStore::copyFile(const char *source, const char *destination) {
  fs::File input = LittleFS.open(source, "r");
  if (!input || !space(StorageKind::Sd, input.size()) ||
      SDFS.exists(destination)) {
    return false;
  }
  const size_t size = input.size();
  fs::File output = SDFS.open(destination, "w");
  if (!output) {
    return false;
  }
  uint8_t buffer[512];
  uint32_t expected = 0xFFFFFFFFu;
  bool ok = true;
  for (size_t offset = 0; offset < size && ok;) {
    const size_t n = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer);
    ok = input.read(buffer, n) == static_cast<int>(n) &&
         output.write(buffer, n) == n;
    if (ok) {
      expected = crc32Update(expected, buffer, n);
    }
    offset += n;
    if (service_ && !service_()) {
      ok = false;
    }
  }
  input.close();
  output.flush();
  output.close();
  input = SDFS.open(destination, "r");
  ok = ok && input && input.size() == size;
  uint32_t actual = 0xFFFFFFFFu;
  for (size_t offset = 0; offset < size && ok;) {
    const size_t n = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer);
    ok = input.read(buffer, n) == static_cast<int>(n);
    if (ok) {
      actual = crc32Update(actual, buffer, n);
    }
    offset += n;
    if (service_ && !service_()) {
      ok = false;
    }
  }
  input.close();
  if (ok && actual == expected) {
    return true;
  }
  if (!removePartial(SDFS, destination)) {
    sdStatus_ = "copy failed AND partial-file removal failed";
  }
  return false;
}

bool RecordingStore::copyToSd(const char *folder, Print &out) {
  begin();
  if (!flashReady_ || !sdReady_ || !space(StorageKind::Sd, 0) ||
      SDFS.exists(folder) || !SDFS.mkdir(folder)) {
    out.println("ERROR: mounts, SD space, or NEW destination folder unavailable.");
    return false;
  }
  const bool ok = walk(StorageKind::LittleFs, "/", 0, out, false, folder);
  out.println(ok ? "Copy verified; originals kept." :
                   "ERROR: copy incomplete; originals kept. Inspect destination.");
  return ok;
}

bool RecordingStore::formatLittleFs() {
  LittleFS.end();
  flashReady_ = false;
  const bool formatted = LittleFS.format();
  if (formatted) {
    flashReady_ = LittleFS.begin();
  }
  flashStatus_ = flashReady_ ? "ready (explicitly formatted)" : "format/mount failed";
  return flashReady_;
}
