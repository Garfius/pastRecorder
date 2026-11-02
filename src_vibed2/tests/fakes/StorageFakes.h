#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../../config.h"

using String = std::string;

class Print {
 public:
  void println(const char *text) { output += text; output += "\n"; }
  void printf(const char *format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    output += buffer;
  }
  std::string output;
};

struct FSInfo {
  uint64_t totalBytes = 0;
  uint64_t usedBytes = 0;
};

namespace fs {
class FS;

class File {
 public:
  struct State {
    std::filesystem::path path;
    FILE *file = nullptr;
    bool shortWrite = false;
    bool corruptRead = false;
    ~State() { if (file) { fclose(file); } }
  };
  File() = default;
  explicit File(std::shared_ptr<State> state) : state_(std::move(state)) {}
  explicit operator bool() const { return static_cast<bool>(state_); }
  bool isDirectory() const {
    return state_ && std::filesystem::is_directory(state_->path);
  }
  size_t size() const { return std::filesystem::file_size(state_->path); }
  size_t write(const uint8_t *bytes, size_t count) {
    const size_t n = state_->shortWrite ? count / 2 : count;
    return fwrite(bytes, 1, n, state_->file);
  }
  int read(uint8_t *bytes, size_t count) {
    const int n = static_cast<int>(fread(bytes, 1, count, state_->file));
    if (n && state_->corruptRead) {
      bytes[0] ^= 0x40;
    }
    return n;
  }
  void flush() { fflush(state_->file); }
  void close() { state_.reset(); }

 private:
  std::shared_ptr<State> state_;
};

class Dir {
 public:
  Dir() = default;
  explicit Dir(const std::filesystem::path &root) {
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
      entries_.push_back(entry);
    }
  }
  bool next() { return index_++ < entries_.size(); }
  String fileName() const { return entries_[index_ - 1].path().filename().string(); }
  bool isDirectory() const { return entries_[index_ - 1].is_directory(); }
  size_t fileSize() const { return entries_[index_ - 1].file_size(); }

 private:
  std::vector<std::filesystem::directory_entry> entries_;
  size_t index_ = 0;
};

class FS {
 public:
  std::filesystem::path root;
  uint64_t capacity = 1u << 24;
  bool failInfo = false;
  bool failCreate = false;
  bool shortWrite = false;
  bool corruptRead = false;
  bool failRename = false;
  bool failRemove = false;

  std::filesystem::path resolve(const char *path) const {
    return root / std::filesystem::path(path).relative_path();
  }
  bool info(FSInfo &info) {
    if (failInfo) {
      return false;
    }
    info.totalBytes = capacity;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file()) {
        info.usedBytes += entry.file_size();
      }
    }
    return true;
  }
  bool exists(const char *path) { return std::filesystem::exists(resolve(path)); }
  File open(const char *path, const char *mode) {
    const auto full = resolve(path);
    const bool writing = *mode == 'w';
    if ((writing && failCreate) || (!writing && !exists(path))) {
      return File{};
    }
    auto state = std::make_shared<File::State>();
    state->path = full;
    state->shortWrite = shortWrite;
    state->corruptRead = corruptRead;
    if (!std::filesystem::is_directory(full)) {
      state->file = fopen(full.c_str(), writing ? "wb" : "rb");
      if (!state->file) {
        return File{};
      }
    }
    return File(state);
  }
  bool remove(const char *path) {
    return !failRemove && std::filesystem::remove(resolve(path));
  }
  bool mkdir(const char *path) {
    return std::filesystem::create_directory(resolve(path));
  }
  bool rmdir(const char *path) { return remove(path); }
  bool rename(const char *from, const char *to) {
    if (failRename) {
      return false;
    }
    std::filesystem::rename(resolve(from), resolve(to));
    return true;
  }
  Dir openDir(const char *path) { return Dir(resolve(path)); }
  void end() {}
};
}

class MockSpi {
 public:
  bool setSCK(unsigned) { return true; }
  bool setRX(unsigned) { return true; }
  bool setTX(unsigned) { return true; }
  void end() {}
};
inline MockSpi SPI;
inline fs::FS SDFS;

struct LittleFSConfig {
  explicit LittleFSConfig(bool) {}
};
class MockLittleFs : public fs::FS {
 public:
  bool mountOk = true;
  bool begin() { return mountOk; }
  bool setConfig(LittleFSConfig) { return true; }
  bool format() {
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
      std::filesystem::remove_all(entry.path());
    }
    mountOk = true;
    return true;
  }
};
inline MockLittleFs LittleFS;

class MockSd {
 public:
  bool mountOk = true;
  bool begin(unsigned, unsigned, MockSpi &) { return mountOk; }
  void end() {}
};
inline MockSd SD;
