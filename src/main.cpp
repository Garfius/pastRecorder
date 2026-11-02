#include <Arduino.h>
#include <EEPROM.h>
#include <I2S.h>
#include <LittleFS.h>
#include <SDFS.h>
#include <SPI.h>
#include <XModem.h>

#include <hardware/adc.h>
#include <malloc.h>
#include <pico/sync.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "DeviceSettings.h"
#include "Mp3ShineEncoder.h"
#include "VoiceCompressor.h"
#include "config.h"

namespace {

static_assert((CFG_PCM_RING_SAMPLES & (CFG_PCM_RING_SAMPLES - 1)) == 0,
              "CFG_PCM_RING_SAMPLES must be a power of two");
static_assert(
    (CFG_RING_PUBLISH_SAMPLES & (CFG_RING_PUBLISH_SAMPLES - 1)) == 0,
    "CFG_RING_PUBLISH_SAMPLES must be a power of two");

enum class FsChoice : uint8_t { Sd, LittleFs };

enum class LedMode : uint8_t {
  Booting,
  Recording,
  Saving,
  Menu,
  ErrorStorage,
  LowBattery,
  Off
};

template <typename T>
T clampValue(T value, T low, T high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

template <typename Allocate, typename Release>
size_t largestAllocation(Allocate allocate, Release release) {
  size_t low = 0;
  size_t high =
      static_cast<size_t>(rp2040.getTotalHeap() + rp2040.getTotalPSRAMHeap());
  while (low < high) {
    const size_t middle = low + (high - low + 1) / 2;
    void *block = allocate(middle);
    if (block) {
      volatile uint8_t *bytes = static_cast<volatile uint8_t *>(block);
      bytes[0] = 0xA5;
      bytes[middle - 1] = 0x5A;
      release(block);
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return low;
}

size_t alignDown(size_t value, size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  return value - (value % alignment);
}

bool parseFloatValue(const String &text, float *out) {
  if (!out) {
    return false;
  }
  char *end = nullptr;
  const float value = strtof(text.c_str(), &end);
  if (end == text.c_str()) {
    return false;
  }
  *out = value;
  return true;
}

bool parseUnsignedValue(const String &text, uint32_t *out) {
  if (!out) {
    return false;
  }
  char *end = nullptr;
  const unsigned long value = strtoul(text.c_str(), &end, 10);
  if (end == text.c_str()) {
    return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

class RgbLed {
 public:
  void begin() {
#if CFG_LED_ENABLE
    pinMode(CFG_LED_R_PIN, OUTPUT);
    pinMode(CFG_LED_G_PIN, OUTPUT);
    pinMode(CFG_LED_B_PIN, OUTPUT);
#endif
    setMode(LedMode::Booting);
  }

  void setMode(LedMode mode) {
    mode_ = mode;
    lastModeChangeMs_ = millis();
    update(true);
  }

  void update(bool force = false) {
#if CFG_LED_ENABLE
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    bool on = true;

    switch (mode_) {
      case LedMode::Booting:
        red = 0;
        green = 0;
        blue = 1;
        on = pulse(1200, 50, force);
        break;
      case LedMode::Recording:
        red = 0;
        green = 1;
        blue = 0;
        on = pulse(900, 30, force);
        break;
      case LedMode::Saving:
        red = 0;
        green = 1;
        blue = 1;
        on = pulse(220, 35, force);
        break;
      case LedMode::Menu:
        red = 0;
        green = 0;
        blue = 1;
        on = true;
        break;
      case LedMode::ErrorStorage:
        red = 1;
        green = 0;
        blue = 0;
        on = pulse(250, 60, force);
        break;
      case LedMode::LowBattery:
        red = 1;
        green = 0;
        blue = 0;
        on = pulse(700, 90, force);
        break;
      case LedMode::Off:
      default:
        red = 0;
        green = 0;
        blue = 0;
        on = true;
        break;
    }

    writePin(CFG_LED_R_PIN, red && on);
    writePin(CFG_LED_G_PIN, green && on);
    writePin(CFG_LED_B_PIN, blue && on);
#else
    (void)force;
#endif
  }

 private:
  bool pulse(uint32_t periodMs, uint32_t onMs, bool force) {
    if (periodMs == 0) {
      return true;
    }
    const uint32_t now = millis();
    if (!force && now == lastUpdateMs_) {
      return lastPulseState_;
    }
    lastUpdateMs_ = now;
    const uint32_t phase = (now - lastModeChangeMs_) % periodMs;
    lastPulseState_ = phase < onMs;
    return lastPulseState_;
  }

  void writePin(uint8_t pin, bool on) {
#if CFG_LED_INVERT_LOGIC
    digitalWrite(pin, on ? LOW : HIGH);
#else
    digitalWrite(pin, on ? HIGH : LOW);
#endif
  }

  LedMode mode_ = LedMode::Off;
  uint32_t lastModeChangeMs_ = 0;
  uint32_t lastUpdateMs_ = 0;
  bool lastPulseState_ = false;
};

class BatteryChecker {
 public:
  void begin(uint16_t thresholdMv, uint32_t intervalMs) {
    thresholdMv_ = thresholdMv;
    intervalMs_ = intervalMs;
    lastCheckMs_ = millis();
    adc_init();
    adc_gpio_init(CFG_BATTERY_ADC_GPIO);
  }

  void setThreshold(uint16_t thresholdMv) { thresholdMv_ = thresholdMv; }

  void setIntervalMs(uint32_t intervalMs) { intervalMs_ = intervalMs; }

  bool shouldCheck(uint32_t nowMs) const {
    return intervalMs_ > 0 && nowMs - lastCheckMs_ >= intervalMs_;
  }

  void markChecked(uint32_t nowMs) { lastCheckMs_ = nowMs; }

  bool isLow(uint16_t mv) const { return mv < thresholdMv_; }

  uint16_t readMilliVolts() const {
    // RP2040-Battery-Check conversion formula.
    adc_select_input(CFG_BATTERY_ADC_INPUT);
    const uint32_t raw = adc_read();
    const int32_t mv = static_cast<int32_t>(raw * 9875UL / (1UL << 12)) - 20;
    if (mv < 0) {
      return 0;
    }
    return static_cast<uint16_t>(mv);
  }

 private:
  uint16_t thresholdMv_ = CFG_BATTERY_LOW_THRESHOLD_MV;
  uint32_t intervalMs_ = CFG_BATTERY_CHECK_INTERVAL_MS;
  uint32_t lastCheckMs_ = 0;
};

class CircularMp3Buffer {
 public:
  ~CircularMp3Buffer() { clear(); }

  void clear() {
    if (data_) {
      free(data_);
      data_ = nullptr;
    }
    capacity_ = 0;
    totalWritten_ = 0;
  }

  void reset() { totalWritten_ = 0; }

  bool allocateWithHeadroom(size_t reserveBytes, size_t minBytes, size_t stepBytes,
                            size_t *largestAtBoot, size_t *chosenSize) {
    clear();

    const size_t largest = largestAllocation(
        [](size_t size) -> void * { return malloc(size); },
        [](void *block) { free(block); });
    if (largestAtBoot) {
      *largestAtBoot = largest;
    }

    if (largest < minBytes) {
      return false;
    }

    size_t candidate = 0;
    if (largest > reserveBytes) {
      candidate = largest - reserveBytes;
    }
    if (candidate < minBytes) {
      candidate = minBytes;
    }
    candidate = alignDown(candidate, stepBytes);
    if (candidate < minBytes) {
      candidate = minBytes;
    }

    for (size_t current = candidate;;) {
      data_ = static_cast<uint8_t *>(malloc(current));
      if (data_) {
        capacity_ = current;
        totalWritten_ = 0;
        if (chosenSize) {
          *chosenSize = current;
        }
        return true;
      }

      if (current <= minBytes || current < stepBytes) {
        break;
      }
      current -= stepBytes;
      if (current < minBytes) {
        current = minBytes;
      }
    }

    capacity_ = 0;
    totalWritten_ = 0;
    if (chosenSize) {
      *chosenSize = 0;
    }
    return false;
  }

  size_t write(const uint8_t *bytes, size_t length) {
    if (!bytes || length == 0) {
      return 0;
    }
    if (!data_ || capacity_ == 0) {
      totalWritten_ += length;
      return length;
    }

    if (length >= capacity_) {
      memcpy(data_, bytes + (length - capacity_), capacity_);
      totalWritten_ += length;
      return length;
    }

    const size_t index = static_cast<size_t>(totalWritten_ % capacity_);
    const size_t firstCopy = (capacity_ - index) < length ? (capacity_ - index)
                                                           : length;
    memcpy(data_ + index, bytes, firstCopy);
    const size_t secondCopy = length - firstCopy;
    if (secondCopy > 0) {
      memcpy(data_, bytes + firstCopy, secondCopy);
    }

    totalWritten_ += length;
    return length;
  }

  size_t size() const {
    if (capacity_ == 0) {
      return 0;
    }
    return totalWritten_ < capacity_ ? static_cast<size_t>(totalWritten_)
                                      : capacity_;
  }

  size_t capacity() const { return capacity_; }

  uint64_t totalWritten() const { return totalWritten_; }

  float bufferedSeconds(uint32_t bitrateKbps) const {
    if (bitrateKbps == 0) {
      return 0.0f;
    }
    const float bits = static_cast<float>(size()) * 8.0f;
    return bits / static_cast<float>(bitrateKbps * 1000UL);
  }

  bool dumpToFile(fs::File &file) const {
    if (!file) {
      return false;
    }

    const size_t used = size();
    if (used == 0) {
      return true;
    }

    if (totalWritten_ <= capacity_) {
      return file.write(data_, used) == used;
    }

    const size_t start = static_cast<size_t>(totalWritten_ % capacity_);
    size_t firstChunk = capacity_ - start;
    if (firstChunk > used) {
      firstChunk = used;
    }
    if (file.write(data_ + start, firstChunk) != firstChunk) {
      return false;
    }
    const size_t secondChunk = used - firstChunk;
    if (secondChunk > 0 && file.write(data_, secondChunk) != secondChunk) {
      return false;
    }
    return true;
  }

 private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
  uint64_t totalWritten_ = 0;
};

class CircularMp3Sink : public Print {
 public:
  explicit CircularMp3Sink(CircularMp3Buffer &buffer) : buffer_(buffer) {}

  using Print::write;

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t *data, size_t length) override {
    // Never return short writes, otherwise upstream encoders may spin forever.
    return buffer_.write(data, length);
  }

 private:
  CircularMp3Buffer &buffer_;
};

class StorageManager {
 public:
  void begin() {
    littleReady_ = LittleFS.begin();

    SPI.setRX(CFG_SD_MISO_PIN);
    SPI.setTX(CFG_SD_MOSI_PIN);
    SPI.setSCK(CFG_SD_SCK_PIN);
    SPI.begin();

    SDFS.setConfig(SDFSConfig(CFG_SD_CS_PIN, SD_SCK_MHZ(CFG_SD_SPI_MHZ), SPI));
    sdReady_ = SDFS.begin();
  }

  void end() {
    if (sdReady_) {
      SDFS.end();
    }
    if (littleReady_) {
      LittleFS.end();
    }
    SPI.end();
  }

  bool littleReady() const { return littleReady_; }

  bool sdReady() const { return sdReady_; }

  fs::FS *getFs(FsChoice choice) {
    if (choice == FsChoice::Sd) {
      return sdReady_ ? &SDFS : nullptr;
    }
    return littleReady_ ? &LittleFS : nullptr;
  }

  const char *name(FsChoice choice) const {
    return choice == FsChoice::Sd ? "SDFS" : "LittleFS";
  }

  uint64_t freeBytes(FsChoice choice) {
    fs::FS *fs = getFs(choice);
    if (!fs) {
      return 0;
    }

    FSInfo info;
    if (!fs->info(info)) {
      return 0;
    }
    return info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
  }

  bool listFiles(FsChoice choice, Print &out) {
    fs::FS *fs = getFs(choice);
    if (!fs) {
      out.println(F("Storage unavailable."));
      return false;
    }
    bool found = listRecursive(*fs, String("/"), 0, out);
    if (!found) {
      out.println(F("  (empty)"));
    }
    return found;
  }

  bool eraseAll(FsChoice choice, Print &out) {
    fs::FS *fs = getFs(choice);
    if (!fs) {
      out.println(F("Storage unavailable."));
      return false;
    }
    return eraseRecursive(*fs, String("/"), out);
  }

  bool copyAllLittleFsToSd(const String &folderName, Print &out) {
    if (!littleReady_) {
      out.println(F("LittleFS unavailable."));
      return false;
    }
    if (!sdReady_) {
      out.println(F("SDFS unavailable."));
      return false;
    }

    String targetRoot = folderName;
    targetRoot.trim();
    if (targetRoot.length() == 0 || targetRoot == "random") {
      targetRoot = String("copy_") + String(rp2040.hwrand32(), HEX);
    }
    if (!targetRoot.startsWith("/")) {
      targetRoot = "/" + targetRoot;
    }

    if (SDFS.exists(targetRoot.c_str())) {
      out.println(F("Target folder already exists on SDFS."));
      return false;
    }
    if (!SDFS.mkdir(targetRoot.c_str())) {
      out.println(F("Could not create target folder on SDFS."));
      return false;
    }

    if (!copyRecursive(LittleFS, SDFS, String("/"), targetRoot, out)) {
      out.println(F("Copy operation failed."));
      return false;
    }

    out.print(F("Copied all LittleFS files to "));
    out.println(targetRoot);
    return true;
  }

  bool saveBufferWithFallback(const CircularMp3Buffer &buffer,
                              uint32_t preferredFileId, uint32_t *savedFileId,
                              FsChoice *savedFs, String *savedPath,
                              String *errorMessage) {
    if (buffer.size() == 0) {
      if (errorMessage) {
        *errorMessage = "Recording buffer is empty.";
      }
      return false;
    }

    uint32_t fileId = preferredFileId == 0 ? 1 : preferredFileId;
    if (trySaveOnFs(FsChoice::Sd, buffer, fileId, savedFileId, savedPath,
                    errorMessage)) {
      if (savedFs) {
        *savedFs = FsChoice::Sd;
      }
      return true;
    }

    if (trySaveOnFs(FsChoice::LittleFs, buffer, fileId, savedFileId, savedPath,
                    errorMessage)) {
      if (savedFs) {
        *savedFs = FsChoice::LittleFs;
      }
      return true;
    }

    if (errorMessage && errorMessage->length() == 0) {
      *errorMessage = "No writable storage available (SDFS and LittleFS failed).";
    }
    return false;
  }

  bool saveBufferToExactPath(FsChoice choice, const CircularMp3Buffer &buffer,
                             const String &path, String *errorMessage) {
    fs::FS *fs = getFs(choice);
    if (!fs) {
      if (errorMessage) {
        *errorMessage = String(name(choice)) + " unavailable.";
      }
      return false;
    }

    if (!hasSpace(*fs, buffer.size() + CFG_STORAGE_MARGIN_BYTES)) {
      if (errorMessage) {
        *errorMessage = String(name(choice)) + " has no free space.";
      }
      return false;
    }

    if (fs->exists(path.c_str())) {
      fs->remove(path.c_str());
    }

    fs::File output = fs->open(path.c_str(), "w");
    if (!output) {
      if (errorMessage) {
        *errorMessage = String("Could not create ") + path + " on " + name(choice);
      }
      return false;
    }

    const bool ok = buffer.dumpToFile(output);
    output.flush();
    const size_t size = output.size();
    output.close();
    if (!ok || size == 0) {
      fs->remove(path.c_str());
      if (errorMessage) {
        *errorMessage = String("Failed writing ") + path + " on " + name(choice);
      }
      return false;
    }

    return true;
  }

 private:
  static String recordingPathForId(uint32_t id) {
    char fileName[24];
    snprintf(fileName, sizeof(fileName), "/rec_%06lu.mp3",
             static_cast<unsigned long>(id));
    return String(fileName);
  }

  bool hasSpace(fs::FS &fs, size_t bytesNeeded) {
    FSInfo info;
    if (!fs.info(info)) {
      return true;
    }
    const uint64_t freeBytes =
        info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
    return freeBytes > bytesNeeded;
  }

  bool trySaveOnFs(FsChoice choice, const CircularMp3Buffer &buffer,
                   uint32_t preferredFileId, uint32_t *savedFileId,
                   String *savedPath, String *errorMessage) {
    fs::FS *fs = getFs(choice);
    if (!fs) {
      return false;
    }

    if (!hasSpace(*fs, buffer.size() + CFG_STORAGE_MARGIN_BYTES)) {
      if (errorMessage) {
        *errorMessage = String(name(choice)) + " is full.";
      }
      return false;
    }

    uint32_t id = preferredFileId == 0 ? 1 : preferredFileId;
    String path;
    bool foundPath = false;
    for (uint16_t attempts = 0; attempts < 2000; ++attempts) {
      path = recordingPathForId(id);
      if (!fs->exists(path.c_str())) {
        foundPath = true;
        break;
      }
      ++id;
    }
    if (!foundPath) {
      if (errorMessage) {
        *errorMessage = String("Could not allocate new file name on ") + name(choice);
      }
      return false;
    }

    fs::File output = fs->open(path.c_str(), "w");
    if (!output) {
      if (errorMessage) {
        *errorMessage = String("Could not create ") + path + " on " + name(choice);
      }
      return false;
    }

    const bool ok = buffer.dumpToFile(output);
    output.flush();
    const size_t bytes = output.size();
    output.close();
    if (!ok || bytes == 0) {
      fs->remove(path.c_str());
      if (errorMessage) {
        *errorMessage = String("Write failed on ") + name(choice);
      }
      return false;
    }

    if (savedFileId) {
      *savedFileId = id;
    }
    if (savedPath) {
      *savedPath = path;
    }
    return true;
  }

  bool listRecursive(fs::FS &fs, const String &path, uint8_t depth, Print &out) {
    bool found = false;
    fs::Dir dir = fs.openDir(path.c_str());
    while (dir.next()) {
      found = true;
      for (uint8_t i = 0; i < depth; ++i) {
        out.print(F("  "));
      }
      out.print(dir.fileName());
      if (dir.isDirectory()) {
        out.println('/');
        String child(path);
        if (!child.endsWith("/")) {
          child += '/';
        }
        child += dir.fileName();
        listRecursive(fs, child, static_cast<uint8_t>(depth + 1), out);
      } else {
        out.print(F("  "));
        out.print(dir.fileSize());
        out.println(F(" bytes"));
      }
    }
    return found;
  }

  bool eraseRecursive(fs::FS &fs, const String &path, Print &out) {
    bool ok = true;
    fs::Dir dir = fs.openDir(path.c_str());
    while (dir.next()) {
      String child(path);
      if (!child.endsWith("/")) {
        child += '/';
      }
      child += dir.fileName();

      if (dir.isDirectory()) {
        ok = eraseRecursive(fs, child, out) && ok;
        if (!fs.rmdir(child.c_str())) {
          out.print(F("Failed to remove dir: "));
          out.println(child);
          ok = false;
        }
      } else {
        if (!fs.remove(child.c_str())) {
          out.print(F("Failed to remove file: "));
          out.println(child);
          ok = false;
        }
      }
    }
    return ok;
  }

  bool copyRecursive(fs::FS &src, fs::FS &dst, const String &srcPath,
                     const String &dstPath, Print &out) {
    fs::Dir dir = src.openDir(srcPath.c_str());
    while (dir.next()) {
      String srcChild(srcPath);
      if (!srcChild.endsWith("/")) {
        srcChild += '/';
      }
      srcChild += dir.fileName();

      String dstChild(dstPath);
      if (!dstChild.endsWith("/")) {
        dstChild += '/';
      }
      dstChild += dir.fileName();

      if (dir.isDirectory()) {
        if (!dst.exists(dstChild.c_str()) && !dst.mkdir(dstChild.c_str())) {
          out.print(F("Could not create directory: "));
          out.println(dstChild);
          return false;
        }
        if (!copyRecursive(src, dst, srcChild, dstChild, out)) {
          return false;
        }
      } else {
        fs::File input = src.open(srcChild.c_str(), "r");
        if (!input) {
          out.print(F("Could not open source file: "));
          out.println(srcChild);
          return false;
        }
        fs::File output = dst.open(dstChild.c_str(), "w");
        if (!output) {
          input.close();
          out.print(F("Could not open destination file: "));
          out.println(dstChild);
          return false;
        }

        uint8_t buffer[512];
        while (true) {
          const int readBytes = input.read(buffer, sizeof(buffer));
          if (readBytes <= 0) {
            break;
          }
          if (output.write(buffer, static_cast<size_t>(readBytes)) !=
              static_cast<size_t>(readBytes)) {
            input.close();
            output.close();
            out.print(F("Write failed for "));
            out.println(dstChild);
            return false;
          }
        }

        output.flush();
        output.close();
        input.close();
      }
    }

    return true;
  }

  bool littleReady_ = false;
  bool sdReady_ = false;
};

class CircularRecorder {
 public:
  CircularRecorder() : microphone_(INPUT), mp3Sink_(mp3Buffer_) {}

  bool initializeMp3Buffer(size_t reserveBytes, size_t minBytes, size_t stepBytes,
                           size_t *largestAtBoot, size_t *chosenSize) {
    return mp3Buffer_.allocateWithHeadroom(reserveBytes, minBytes, stepBytes,
                                           largestAtBoot, chosenSize);
  }

  void initCoreSync() {
    sem_init(&encodeStart_, 0, 1);
    sem_init(&encodeDone_, 0, 1);
    __mem_fence_release();
    coreSyncReady_ = true;
  }

  bool coreSyncReady() const { return coreSyncReady_; }

  bool start(const DeviceSettings &settings) {
    if (running_) {
      return true;
    }
    if (mp3Buffer_.capacity() == 0) {
      lastError_ = F("MP3 ring buffer is not allocated.");
      return false;
    }

    if (!configureMicrophone()) {
      lastError_ = F("Could not configure I2S microphone.");
      return false;
    }

    mp3Buffer_.reset();
    lastError_ = String();
    droppedSamples_ = 0;
    localHead_ = 0;

    ringHeadPublished_ = 0;
    ringTailPublished_ = 0;
    producerDone_ = false;
    encoderFailed_ = false;
    core1IdleUs_ = 0;
    core1TotalUs_ = 0;

    VoiceCompressorParams params = {};
    params.highPassHz = settings.highPassHz;
    params.inputGain = settings.inputGain;
    params.makeupGain = settings.makeupGain;
    params.ceiling = settings.ceiling;
    params.attackMs = settings.attackMs;
    params.releaseMs = settings.releaseMs;
    params.threshold = settings.threshold;
    params.ratio = settings.ratio;
    voiceCompressorBegin(CFG_AUDIO_SAMPLE_RATE, params);

    while (sem_try_acquire(&encodeDone_)) {
    }

    if (!microphone_.begin()) {
      lastError_ = F("Could not start I2S microphone.");
      return false;
    }

    sem_release(&encodeStart_);
    startedAtMs_ = millis();
    running_ = true;
    return true;
  }

  void serviceCapture() {
    if (!running_) {
      return;
    }

    int32_t leftSample = 0;
    int32_t rightSample = 0;
    if (!microphone_.read32(&leftSample, &rightSample)) {
      lastError_ = F("I2S read failed.");
      stop();
      return;
    }

    int32_t sample = CFG_I2S_USE_RIGHT_CHANNEL ? rightSample : leftSample;
    sample = voiceCompressorProcess(sample);

    __mem_fence_acquire();
    const uint32_t tail = ringTailPublished_;
    const uint32_t queued = localHead_ - tail;
    if (queued >= CFG_PCM_RING_SAMPLES) {
      ++droppedSamples_;
      return;
    }

    pcmRing_[localHead_ & (CFG_PCM_RING_SAMPLES - 1)] =
        static_cast<int16_t>(sample >> 16);
    ++localHead_;

    if ((localHead_ & (CFG_RING_PUBLISH_SAMPLES - 1)) == 0) {
      __mem_fence_release();
      ringHeadPublished_ = localHead_;
    }

    if (encoderFailed_) {
      lastError_ = F("MP3 encoder failed.");
      stop();
    }
  }

  bool stop() {
    if (!running_) {
      return true;
    }

    microphone_.end();

    __mem_fence_release();
    ringHeadPublished_ = localHead_;
    __mem_fence_release();
    producerDone_ = true;

    bool workerStopped = false;
    const uint32_t waitStartMs = millis();
    while (!workerStopped) {
      if (sem_try_acquire(&encodeDone_)) {
        workerStopped = true;
        break;
      }

      if (millis() - waitStartMs >= CFG_ENCODER_STOP_TIMEOUT_MS) {
        encoderFailed_ = true;
        if (lastError_.length() == 0) {
          lastError_ = F("Encoder stop timeout.");
        }
        break;
      }
      delay(1);
    }

    running_ = false;
    if (encoderFailed_ && lastError_.length() == 0) {
      lastError_ = F("MP3 encoder failed.");
    }
    return workerStopped && !encoderFailed_;
  }

  void core1Loop() {
    sem_acquire_blocking(&encodeStart_);
    runCore1Encoding();
    sem_release(&encodeDone_);
  }

  bool isRunning() const { return running_; }

  uint32_t droppedSamples() const { return droppedSamples_; }

  const String &lastError() const { return lastError_; }

  CircularMp3Buffer &buffer() { return mp3Buffer_; }

  const CircularMp3Buffer &buffer() const { return mp3Buffer_; }

  uint32_t core1IdleUs() const { return core1IdleUs_; }

  uint32_t core1TotalUs() const { return core1TotalUs_; }

  uint32_t uptimeMs() const { return millis() - startedAtMs_; }

 private:
  bool configureMicrophone() {
    return microphone_.setBCLK(CFG_I2S_BCLK_PIN) &&
           microphone_.setDATA(CFG_I2S_DATA_PIN) &&
           microphone_.setBitsPerSample(32) &&
           microphone_.setFrequency(CFG_AUDIO_SAMPLE_RATE) &&
           microphone_.setBuffers(CFG_MIC_BUFFER_COUNT, CFG_MIC_BUFFER_BYTES);
  }

  bool writePcmToEncoder(Mp3ShineEncoder &encoder, int16_t sample,
                         size_t *stagedBytes) {
    if (!stagedBytes) {
      return false;
    }

    pcmStage_[(*stagedBytes)++] = static_cast<uint8_t>(sample & 0xFF);
    pcmStage_[(*stagedBytes)++] =
        static_cast<uint8_t>((static_cast<uint16_t>(sample) >> 8) & 0xFF);

    if (*stagedBytes == sizeof(pcmStage_)) {
      if (encoder.write(pcmStage_, *stagedBytes) != *stagedBytes) {
        return false;
      }
      *stagedBytes = 0;
    }

    return true;
  }

  void runCore1Encoding() {
    Mp3ShineEncoder encoder(mp3Sink_, CFG_AUDIO_SAMPLE_RATE,
                            CFG_MP3_BITRATE_KBPS);
    bool encodingOk = encoder.begin();

    size_t stagedBytes = 0;
    uint32_t tail = 0;
    uint32_t idleUs = 0;
    const uint32_t startUs = micros();

    while (true) {
      if (!encodingOk) {
        encoderFailed_ = true;
        break;
      }
      const bool producerFinished = producerDone_;
      __mem_fence_acquire();
      const uint32_t head = ringHeadPublished_;
      __mem_fence_acquire();
      uint32_t pending = head - tail;
      if (pending == 0) {
        if (producerFinished) {
          break;
        }
        const uint32_t idleStartUs = micros();
        delay(1);
        idleUs += micros() - idleStartUs;
        continue;
      }

      if (pending > CFG_RING_CONSUME_SAMPLES) {
        pending = CFG_RING_CONSUME_SAMPLES;
      }

      for (uint32_t index = 0; index < pending && encodingOk; ++index) {
        const int16_t sample = pcmRing_[tail++ & (CFG_PCM_RING_SAMPLES - 1)];
        encodingOk = writePcmToEncoder(encoder, sample, &stagedBytes);
      }

      __mem_fence_release();
      ringTailPublished_ = tail;
    }

    if (encodingOk && stagedBytes > 0) {
      encodingOk = encoder.write(pcmStage_, stagedBytes) == stagedBytes;
      stagedBytes = 0;
    }

    if (!encoder.end()) {
      encodingOk = false;
    }
    if (!encodingOk) {
      encoderFailed_ = true;
    }

    core1IdleUs_ = idleUs;
    core1TotalUs_ = micros() - startUs;
  }

  I2S microphone_;

  int16_t pcmRing_[CFG_PCM_RING_SAMPLES];
  volatile uint32_t ringHeadPublished_ = 0;
  volatile uint32_t ringTailPublished_ = 0;
  volatile bool producerDone_ = false;
  volatile bool encoderFailed_ = false;
  volatile bool coreSyncReady_ = false;
  volatile uint32_t core1IdleUs_ = 0;
  volatile uint32_t core1TotalUs_ = 0;

  semaphore_t encodeStart_;
  semaphore_t encodeDone_;

  uint8_t pcmStage_[CFG_MP3_STAGE_BYTES];
  CircularMp3Buffer mp3Buffer_;
  CircularMp3Sink mp3Sink_;

  bool running_ = false;
  uint32_t localHead_ = 0;
  uint32_t droppedSamples_ = 0;
  uint32_t startedAtMs_ = 0;
  String lastError_;
};

class App;
App *gApp = nullptr;

bool onXmodemUpdateForwarder(uint8_t code, uint8_t value);

class App {
 public:
  void setup() {
    gApp = this;

    pinMode(CFG_BUTTON_PIN, INPUT_PULLUP);
    buttonRawPressed_ = digitalRead(CFG_BUTTON_PIN) == LOW;
    buttonLastEdgeMs_ = millis();
    attachInterrupt(digitalPinToInterrupt(CFG_BUTTON_PIN), onButtonChange, CHANGE);

    led_.begin();
    led_.setMode(LedMode::Booting);

    CFG_SERIAL_PORT.begin(CFG_SERIAL_BAUD);
    
    myXModem.onXmodemUpdate(onXmodemUpdateForwarder);

    if (!settingsStore_.begin() || !settingsStore_.load(&settings_)) {
      settings_ = DeviceSettingsStore::defaults();
      settingsStore_.save(settings_);
    }

    battery_.begin(settings_.lowBatteryThresholdMv,
                   settings_.batteryCheckIntervalMs);
    storage_.begin();

    const uint16_t bootBatteryMv = battery_.readMilliVolts();
    lastBatteryMv_ = bootBatteryMv;

    const uint16_t rechargeThreshold =
        static_cast<uint16_t>(settings_.lowBatteryThresholdMv +
                              settings_.rechargeThresholdMv);
    if (settings_.lowBatteryShutdown && bootBatteryMv < rechargeThreshold) {
      led_.setMode(LedMode::LowBattery);
      powerOff(true);
      return;
    }

    if (settings_.lowBatteryShutdown && bootBatteryMv >= rechargeThreshold) {
      settings_.lowBatteryShutdown = 0;
      settingsStore_.save(settings_);
    }

    recorder_.initializeMp3Buffer(CFG_MP3_RAM_RESERVE_BYTES,
                                  CFG_MP3_RAM_MIN_BYTES,
                                  CFG_MP3_RAM_ALLOC_STEP_BYTES,
                                  &largestAllocAtBoot_, &mp3RingCapacityAtBoot_);

    recorder_.initCoreSync();

    if (mp3RingCapacityAtBoot_ == 0) {
      enterMenu("Could not allocate MP3 circular RAM buffer.");
      return;
    }

    if (!startRecorderWithRecovery("boot")) {
      enterMenu("Could not start recorder.");
      return;
    }

    led_.setMode(LedMode::Recording);
  }

  void setupCore1() {
    while (!recorder_.coreSyncReady()) {
      delay(1);
    }
    __mem_fence_acquire();
  }

  void loopCore1() { recorder_.core1Loop(); }

  void loop() {
    if (inMenu_) {
      serviceMenuHousekeeping();
      return;
    }

    if (recorder_.isRunning()) {
      recorder_.serviceCapture();
      if (!recorder_.isRunning()) {
        enterMenu(recorder_.lastError().c_str());
        return;
      }
    }

    const uint32_t nowMs = millis();
    led_.update();
    serviceButton(nowMs);
    checkBattery(nowMs);

    if (CFG_SERIAL_PORT.available() > 0) {
      enterMenu("Serial activity detected.");
      return;
    }

    if (!recorder_.isRunning()) {
      if (!startRecorderWithRecovery("loop")) {
        enterMenu("Recorder stopped and could not restart.");
      } else {
        led_.setMode(LedMode::Recording);
      }
    }
  }

  bool onXmodemUpdate(uint8_t code, uint8_t value) {
    if (!serialUnlocked_) {
      return true;
    }

    if (code == 0) {
      if ((value & 0x0F) == 0) {
        CFG_SERIAL_PORT.print('.');
      }
      return true;
    }

    if (code == 1 && value == 2) {
      CFG_SERIAL_PORT.println(F("XModem: file not found."));
      return true;
    }
    if (code == 2) {
      CFG_SERIAL_PORT.println(F("XModem: storage error."));
      return true;
    }
    if (code == 3 && value == 3) {
      CFG_SERIAL_PORT.println(F("XModem: retry limit reached."));
      return true;
    }

    return true;
  }

 private:
  bool startRecorderWithRecovery(const char *contextMessage) {
    if (recorder_.start(settings_)) {
      return true;
    }

    size_t largest = 0;
    size_t chosen = 0;
    const size_t recoveryMin =
        CFG_MP3_RAM_RECOVERY_MIN_BYTES < CFG_MP3_RAM_MIN_BYTES
            ? CFG_MP3_RAM_RECOVERY_MIN_BYTES
            : CFG_MP3_RAM_MIN_BYTES;
    const size_t recoveryReserve =
        CFG_MP3_RAM_RESERVE_BYTES + CFG_MP3_RAM_RECOVERY_EXTRA_RESERVE_BYTES;

    if (!recorder_.initializeMp3Buffer(recoveryReserve, recoveryMin,
                                       CFG_MP3_RAM_ALLOC_STEP_BYTES, &largest,
                                       &chosen)) {
      if (serialUnlocked_) {
        CFG_SERIAL_PORT.println(F("Recorder recovery allocation failed."));
      }
      return false;
    }

    largestAllocAtBoot_ = largest;
    mp3RingCapacityAtBoot_ = chosen;

    if (serialUnlocked_) {
      CFG_SERIAL_PORT.print(F("Retrying recorder start after recovery ("));
      CFG_SERIAL_PORT.print(contextMessage);
      CFG_SERIAL_PORT.print(F(") ring="));
      CFG_SERIAL_PORT.print(static_cast<unsigned long>(chosen));
      CFG_SERIAL_PORT.print(F(" reserve="));
      CFG_SERIAL_PORT.println(static_cast<unsigned long>(recoveryReserve));
    }

    return recorder_.start(settings_);
  }

  static void onButtonChange() {
    gApp->buttonRawPressed_ = digitalRead(CFG_BUTTON_PIN) == LOW;
    gApp->buttonLastEdgeMs_ = millis();
  }

  void logButtonEvent(const __FlashStringHelper *event, uint32_t heldMs = 0) {
    CFG_SERIAL_PORT.print(F("[button t="));
    CFG_SERIAL_PORT.print(millis());
    CFG_SERIAL_PORT.print(F(" ms] "));
    CFG_SERIAL_PORT.print(event);
    CFG_SERIAL_PORT.print(F(" held_ms="));
    CFG_SERIAL_PORT.print(heldMs);
    CFG_SERIAL_PORT.print(F(" menu="));
    CFG_SERIAL_PORT.print(inMenu_ ? 1 : 0);
    CFG_SERIAL_PORT.print(F(" recording="));
    CFG_SERIAL_PORT.println(recorder_.isRunning() ? 1 : 0);
  }

  void logStoppedRecording() {
    CFG_SERIAL_PORT.print(F("[button] MP3 bytes="));
    CFG_SERIAL_PORT.print(recorder_.buffer().size());
    CFG_SERIAL_PORT.print(F(" capacity="));
    CFG_SERIAL_PORT.print(recorder_.buffer().capacity());
    CFG_SERIAL_PORT.print(F(" buffered_seconds="));
    CFG_SERIAL_PORT.println(
        recorder_.buffer().bufferedSeconds(CFG_MP3_BITRATE_KBPS), 2);
    CFG_SERIAL_PORT.print(F("[button] Dropped PCM samples="));
    CFG_SERIAL_PORT.print(recorder_.droppedSamples());
    CFG_SERIAL_PORT.print(F(" core1_idle_us="));
    CFG_SERIAL_PORT.print(recorder_.core1IdleUs());
    CFG_SERIAL_PORT.print(F(" core1_total_us="));
    CFG_SERIAL_PORT.println(recorder_.core1TotalUs());
    CFG_SERIAL_PORT.print(F("[button] Storage SD="));
    CFG_SERIAL_PORT.print(storage_.sdReady() ? F("ready") : F("unavailable"));
    CFG_SERIAL_PORT.print(F(" LittleFS="));
    CFG_SERIAL_PORT.println(storage_.littleReady() ? F("ready") : F("unavailable"));
    if (recorder_.lastError().length() > 0) {
      CFG_SERIAL_PORT.print(F("[button] Recorder error: "));
      CFG_SERIAL_PORT.println(recorder_.lastError());
    }
  }

  void serviceButton(uint32_t nowMs) {
    const uint32_t interruptState = save_and_disable_interrupts();
    const bool rawPressed = buttonRawPressed_;
    const uint32_t lastEdgeMs = buttonLastEdgeMs_;
    restore_interrupts(interruptState);

    nowMs = millis();
    if (nowMs - lastEdgeMs < CFG_BUTTON_DEBOUNCE_MS) {
      return;
    }

    if (rawPressed != buttonWasDown_) {
      buttonWasDown_ = rawPressed;
      if (rawPressed) {
        buttonDownAtMs_ = lastEdgeMs;
        longPressHandled_ = false;
        logButtonEvent(F("pressed (debounced, active-low)"));
      } else {
        const uint32_t heldMs = lastEdgeMs - buttonDownAtMs_;
        logButtonEvent(F("released (debounced)"), heldMs);
        if (!longPressHandled_) {
          if (heldMs >= CFG_LONG_PRESS_POWER_OFF_MS) {
            longPressHandled_ = true;
            logButtonEvent(F("long press: power off"), heldMs);
            powerOff(false);
          } else if (!inMenu_) {
            logButtonEvent(F("short press: save recording"), heldMs);
            handleButtonShortPress();
          } else {
            logButtonEvent(F("short press ignored in menu"), heldMs);
          }
        }
      }
    }

    if (buttonWasDown_ && !longPressHandled_ &&
        nowMs - buttonDownAtMs_ >= CFG_LONG_PRESS_POWER_OFF_MS) {
      longPressHandled_ = true;
      logButtonEvent(F("long press: power off"), nowMs - buttonDownAtMs_);
      powerOff(false);
    }
  }

  void checkBattery(uint32_t nowMs) {
    if (!battery_.shouldCheck(nowMs)) {
      return;
    }
    battery_.markChecked(nowMs);

    const uint16_t batteryMv = battery_.readMilliVolts();
    lastBatteryMv_ = batteryMv;
    if (battery_.isLow(batteryMv)) {
      settings_.lowBatteryShutdown = 1;
      settingsStore_.save(settings_);
      led_.setMode(LedMode::LowBattery);
      powerOff(true);
    }
  }

  void handleButtonShortPress() {
    if (!recorder_.isRunning()) {
      logButtonEvent(F("save skipped: recorder is not running"));
      return;
    }

    led_.setMode(LedMode::Saving);
    const uint32_t stopStartedMs = millis();
    logButtonEvent(F("stopping recorder and flushing Shine"));
    const bool stopped = recorder_.stop();
    CFG_SERIAL_PORT.print(F("[button] Recorder stop "));
    CFG_SERIAL_PORT.print(stopped ? F("OK") : F("FAILED"));
    CFG_SERIAL_PORT.print(F(" elapsed_ms="));
    CFG_SERIAL_PORT.println(millis() - stopStartedMs);
    if (!stopped) {
      CFG_SERIAL_PORT.print(F("[button] Recorder error: "));
      CFG_SERIAL_PORT.println(recorder_.lastError());
      enterMenu(recorder_.lastError().c_str());
      return;
    }
    logStoppedRecording();

    uint32_t savedFileId = 0;
    FsChoice usedFs = FsChoice::LittleFs;
    String savedPath;
    String error;
    const uint32_t saveStartedMs = millis();
    CFG_SERIAL_PORT.print(F("[button] Saving with storage fallback; next_file_id="));
    CFG_SERIAL_PORT.println(settings_.nextFileId);
    if (!storage_.saveBufferWithFallback(recorder_.buffer(), settings_.nextFileId,
                                         &savedFileId, &usedFs, &savedPath,
                                         &error)) {
      CFG_SERIAL_PORT.print(F("[button] Save FAILED: "));
      CFG_SERIAL_PORT.println(error);
      led_.setMode(LedMode::ErrorStorage);
      enterMenu(error.c_str());
      return;
    }

    settings_.nextFileId = savedFileId + 1;
    const bool settingsSaved = settingsStore_.save(settings_);

    CFG_SERIAL_PORT.print(F("[button] Saved "));
    CFG_SERIAL_PORT.print(savedPath);
    CFG_SERIAL_PORT.print(F(" to "));
    CFG_SERIAL_PORT.print(storage_.name(usedFs));
    CFG_SERIAL_PORT.print(F(" bytes="));
    CFG_SERIAL_PORT.print(recorder_.buffer().size());
    CFG_SERIAL_PORT.print(F(" elapsed_ms="));
    CFG_SERIAL_PORT.println(millis() - saveStartedMs);
    CFG_SERIAL_PORT.print(F("[button] Persist next_file_id="));
    CFG_SERIAL_PORT.print(settings_.nextFileId);
    CFG_SERIAL_PORT.println(settingsSaved ? F(" OK") : F(" FAILED"));

    logButtonEvent(F("restarting recorder"));
    if (!startRecorderWithRecovery("resume")) {
      CFG_SERIAL_PORT.print(F("[button] Recorder restart FAILED: "));
      CFG_SERIAL_PORT.println(recorder_.lastError());
      enterMenu("Could not restart recorder after save.");
      return;
    }
    led_.setMode(LedMode::Recording);
    logButtonEvent(F("capture resumed; encoder startup is asynchronous"));
  }

  void powerOff(bool dueToLowBattery) {
    if (dueToLowBattery) {
      settings_.lowBatteryShutdown = 1;
      settingsStore_.save(settings_);
    }

    const bool stopped = recorder_.stop();
    if (!dueToLowBattery) {
      CFG_SERIAL_PORT.print(F("[button] Shutdown recorder stop "));
      CFG_SERIAL_PORT.println(stopped ? F("OK") : F("FAILED"));
      if (stopped) {
        logStoppedRecording();
      } else {
        CFG_SERIAL_PORT.print(F("[button] Recorder error: "));
        CFG_SERIAL_PORT.println(recorder_.lastError());
      }
      CFG_SERIAL_PORT.println(
          F("[button] Closing storage; RAM recording is not saved on long press."));
    }
    storage_.end();

    led_.setMode(dueToLowBattery ? LedMode::LowBattery : LedMode::Off);
    led_.update(true);

    if (!dueToLowBattery) {
      CFG_SERIAL_PORT.println(F("[button] Power off: flushing serial and entering WFI."));
    }
    CFG_SERIAL_PORT.flush();
    CFG_SERIAL_PORT.end();

    while (true) {
      __wfi();
    }
  }

  void enterMenu(const char *reason) {
    if (inMenu_) {
      return;
    }

    inMenu_ = true;
    serialUnlocked_ = true;

    recorder_.stop();
    led_.setMode(LedMode::Menu);

    while (CFG_SERIAL_PORT.available()) {
      CFG_SERIAL_PORT.read();
    }

    CFG_SERIAL_PORT.println();
    CFG_SERIAL_PORT.println(F("Past Recorder Circular Mode"));
    CFG_SERIAL_PORT.println(F("No wiretaps, just circular buffers."));
    if (reason && reason[0]) {
      CFG_SERIAL_PORT.print(F("Reason: "));
      CFG_SERIAL_PORT.println(reason);
    }

    showDashboard();
    runMenuLoop();
  }

  void showDashboard() {
    CFG_SERIAL_PORT.println();
    CFG_SERIAL_PORT.println(F("Status"));
    CFG_SERIAL_PORT.print(F("  Battery: "));
    CFG_SERIAL_PORT.print(lastBatteryMv_);
    CFG_SERIAL_PORT.println(F(" mV"));

    CFG_SERIAL_PORT.print(F("  Storage SD: "));
    CFG_SERIAL_PORT.println(storage_.sdReady() ? F("ready") : F("unavailable"));
    CFG_SERIAL_PORT.print(F("  Storage LittleFS: "));
    CFG_SERIAL_PORT.println(storage_.littleReady() ? F("ready") : F("unavailable"));

    CFG_SERIAL_PORT.print(F("  SD free: "));
    CFG_SERIAL_PORT.print(static_cast<unsigned long long>(storage_.freeBytes(FsChoice::Sd)));
    CFG_SERIAL_PORT.println(F(" bytes"));
    CFG_SERIAL_PORT.print(F("  LittleFS free: "));
    CFG_SERIAL_PORT.print(
        static_cast<unsigned long long>(storage_.freeBytes(FsChoice::LittleFs)));
    CFG_SERIAL_PORT.println(F(" bytes"));

    CFG_SERIAL_PORT.print(F("  MP3 ring capacity: "));
    CFG_SERIAL_PORT.print(recorder_.buffer().capacity());
    CFG_SERIAL_PORT.print(F(" bytes (~"));
    CFG_SERIAL_PORT.print(recorder_.buffer().capacity() * 8 / (CFG_MP3_BITRATE_KBPS * 1000UL));
    CFG_SERIAL_PORT.println(F(" s @ configured bitrate)"));

    CFG_SERIAL_PORT.print(F("  MP3 ring currently buffered: "));
    CFG_SERIAL_PORT.print(recorder_.buffer().size());
    CFG_SERIAL_PORT.print(F(" bytes (~"));
    CFG_SERIAL_PORT.print(recorder_.buffer().bufferedSeconds(CFG_MP3_BITRATE_KBPS), 1);
    CFG_SERIAL_PORT.println(F(" s)"));

    CFG_SERIAL_PORT.print(F("  Dropped PCM samples: "));
    CFG_SERIAL_PORT.println(recorder_.droppedSamples());

    CFG_SERIAL_PORT.print(F("  Core1 encode idle: "));
    CFG_SERIAL_PORT.print(recorder_.core1IdleUs() / 1000);
    CFG_SERIAL_PORT.print(F(" ms / "));
    CFG_SERIAL_PORT.print(recorder_.core1TotalUs() / 1000);
    CFG_SERIAL_PORT.println(F(" ms"));

    CFG_SERIAL_PORT.print(F("  Largest malloc block at boot: "));
    CFG_SERIAL_PORT.print(static_cast<unsigned long>(largestAllocAtBoot_));
    CFG_SERIAL_PORT.println(F(" bytes"));

    CFG_SERIAL_PORT.print(F("  Allocated MP3 ring at boot: "));
    CFG_SERIAL_PORT.print(static_cast<unsigned long>(mp3RingCapacityAtBoot_));
    CFG_SERIAL_PORT.println(F(" bytes"));

    CFG_SERIAL_PORT.println();
    CFG_SERIAL_PORT.println(F("Audio/compressor settings"));
    CFG_SERIAL_PORT.print(F("  highPassHz="));
    CFG_SERIAL_PORT.println(settings_.highPassHz, 2);
    CFG_SERIAL_PORT.print(F("  inputGain="));
    CFG_SERIAL_PORT.println(settings_.inputGain, 2);
    CFG_SERIAL_PORT.print(F("  makeupGain="));
    CFG_SERIAL_PORT.println(settings_.makeupGain, 2);
    CFG_SERIAL_PORT.print(F("  ceiling="));
    CFG_SERIAL_PORT.println(settings_.ceiling, 3);
    CFG_SERIAL_PORT.print(F("  attackMs="));
    CFG_SERIAL_PORT.println(settings_.attackMs, 2);
    CFG_SERIAL_PORT.print(F("  releaseMs="));
    CFG_SERIAL_PORT.println(settings_.releaseMs, 2);
    CFG_SERIAL_PORT.print(F("  threshold="));
    CFG_SERIAL_PORT.println(settings_.threshold, 3);
    CFG_SERIAL_PORT.print(F("  ratio="));
    CFG_SERIAL_PORT.println(settings_.ratio, 2);

    CFG_SERIAL_PORT.print(F("  lowBatteryThresholdMv="));
    CFG_SERIAL_PORT.println(settings_.lowBatteryThresholdMv);
    CFG_SERIAL_PORT.print(F("  rechargeThresholdMv="));
    CFG_SERIAL_PORT.println(settings_.rechargeThresholdMv);
    CFG_SERIAL_PORT.print(F("  batteryCheckIntervalMs="));
    CFG_SERIAL_PORT.println(settings_.batteryCheckIntervalMs);
    CFG_SERIAL_PORT.print(F("  nextFileId="));
    CFG_SERIAL_PORT.println(settings_.nextFileId);
  }

  void runMenuLoop() {
    while (true) {
      CFG_SERIAL_PORT.println();
      CFG_SERIAL_PORT.println(F("Menu options"));
      CFG_SERIAL_PORT.println(F("  1 - List files (choose LittleFS or SD)"));
      CFG_SERIAL_PORT.println(F("  2 - Send file via XModem (choose LittleFS or SD)"));
      CFG_SERIAL_PORT.println(F("  3 - Erase all files (choose LittleFS or SD)"));
      CFG_SERIAL_PORT.println(
          F("  4 - Copy all LittleFS files to a new microSD folder (name or random)"));
      CFG_SERIAL_PORT.println(F("  5 - Audio quality settings (save to EEPROM)"));
      CFG_SERIAL_PORT.println(F("  6 - Record audio sample and send it via XModem"));
      CFG_SERIAL_PORT.println(F("  7 - Send LittleFS file via XModem"));
      CFG_SERIAL_PORT.println(F("  8 - Exit menu, sleep and reboot"));
      CFG_SERIAL_PORT.print(F("> "));

      const String choice = readLineBlocking();
      if (choice == "1") {
        menuListFiles();
      } else if (choice == "2") {
        menuSendFileAnyFs();
      } else if (choice == "3") {
        menuEraseAll();
      } else if (choice == "4") {
        menuCopyLittleFsToSd();
      } else if (choice == "5") {
        menuAudioSettings();
      } else if (choice == "6") {
        menuRecordSampleAndSend();
      } else if (choice == "7") {
        menuSendLittleFsFile();
      } else if (choice == "8") {
        menuExitAndReboot();
      } else {
        CFG_SERIAL_PORT.println(F("Invalid option. Showing status again."));
        showDashboard();
      }
    }
  }

  void menuListFiles() {
    FsChoice choice;
    if (!promptFsChoice(&choice)) {
      CFG_SERIAL_PORT.println(F("Invalid storage selection."));
      return;
    }
    CFG_SERIAL_PORT.print(F("Listing "));
    CFG_SERIAL_PORT.println(storage_.name(choice));
    storage_.listFiles(choice, CFG_SERIAL_PORT);
  }

  void menuSendFileAnyFs() {
    FsChoice choice;
    if (!promptFsChoice(&choice)) {
      CFG_SERIAL_PORT.println(F("Invalid storage selection."));
      return;
    }

    CFG_SERIAL_PORT.print(F("Enter path on "));
    CFG_SERIAL_PORT.print(storage_.name(choice));
    CFG_SERIAL_PORT.print(F(" (example /rec_000001.mp3): "));
    const String path = readLineBlocking();
    if (path.length() == 0) {
      CFG_SERIAL_PORT.println(F("Empty path."));
      return;
    }
    sendFileViaXmodem(choice, path);
  }

  void menuEraseAll() {
    FsChoice choice;
    if (!promptFsChoice(&choice)) {
      CFG_SERIAL_PORT.println(F("Invalid storage selection."));
      return;
    }

    CFG_SERIAL_PORT.print(F("Erase all files on "));
    CFG_SERIAL_PORT.print(storage_.name(choice));
    CFG_SERIAL_PORT.print(F("? Type YES to confirm: "));
    const String confirm = readLineBlocking();
    if (confirm != "YES") {
      CFG_SERIAL_PORT.println(F("Cancelled."));
      return;
    }

    const bool ok = storage_.eraseAll(choice, CFG_SERIAL_PORT);
    CFG_SERIAL_PORT.println(ok ? F("Erase complete.") : F("Erase finished with errors."));
  }

  void menuCopyLittleFsToSd() {
    CFG_SERIAL_PORT.print(F("Target folder name on SD (empty or random): "));
    const String folder = readLineBlocking();
    storage_.copyAllLittleFsToSd(folder, CFG_SERIAL_PORT);
  }

  void menuAudioSettings() {
    while (true) {
      CFG_SERIAL_PORT.println();
      CFG_SERIAL_PORT.println(F("Audio quality settings"));
      CFG_SERIAL_PORT.println(F("  1 - highPassHz"));
      CFG_SERIAL_PORT.println(F("  2 - inputGain"));
      CFG_SERIAL_PORT.println(F("  3 - makeupGain"));
      CFG_SERIAL_PORT.println(F("  4 - ceiling"));
      CFG_SERIAL_PORT.println(F("  5 - attackMs"));
      CFG_SERIAL_PORT.println(F("  6 - releaseMs"));
      CFG_SERIAL_PORT.println(F("  7 - threshold"));
      CFG_SERIAL_PORT.println(F("  8 - ratio"));
      CFG_SERIAL_PORT.println(F("  9 - lowBatteryThresholdMv"));
      CFG_SERIAL_PORT.println(F(" 10 - rechargeThresholdMv"));
      CFG_SERIAL_PORT.println(F(" 11 - batteryCheckIntervalMs"));
      CFG_SERIAL_PORT.println(F("  x - back to main menu"));
      CFG_SERIAL_PORT.print(F("> "));

      const String item = readLineBlocking();
      if (item == "x" || item == "X") {
        return;
      }

      uint32_t option = 0;
      if (!parseUnsignedValue(item, &option)) {
        CFG_SERIAL_PORT.println(F("Invalid selection."));
        continue;
      }

      CFG_SERIAL_PORT.print(F("New value: "));
      const String valueText = readLineBlocking();

      bool updated = false;
      float floatValue = 0.0f;
      uint32_t intValue = 0;
      switch (option) {
        case 1:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.highPassHz = clampValue(floatValue, 20.0f, 2000.0f);
            updated = true;
          }
          break;
        case 2:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.inputGain = clampValue(floatValue, 0.1f, 40.0f);
            updated = true;
          }
          break;
        case 3:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.makeupGain = clampValue(floatValue, 0.1f, 40.0f);
            updated = true;
          }
          break;
        case 4:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.ceiling = clampValue(floatValue, 0.1f, 0.9999f);
            updated = true;
          }
          break;
        case 5:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.attackMs = clampValue(floatValue, 1.0f, 2000.0f);
            updated = true;
          }
          break;
        case 6:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.releaseMs = clampValue(floatValue, 1.0f, 5000.0f);
            updated = true;
          }
          break;
        case 7:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.threshold = clampValue(floatValue, 0.0001f, 1.0f);
            updated = true;
          }
          break;
        case 8:
          if (parseFloatValue(valueText, &floatValue)) {
            settings_.ratio = clampValue(floatValue, 1.0f, 50.0f);
            updated = true;
          }
          break;
        case 9:
          if (parseUnsignedValue(valueText, &intValue)) {
            settings_.lowBatteryThresholdMv =
                static_cast<uint16_t>(clampValue(intValue, 2500UL, 4300UL));
            battery_.setThreshold(settings_.lowBatteryThresholdMv);
            updated = true;
          }
          break;
        case 10:
          if (parseUnsignedValue(valueText, &intValue)) {
            settings_.rechargeThresholdMv =
                static_cast<uint16_t>(clampValue(intValue, 0UL, 1000UL));
            updated = true;
          }
          break;
        case 11:
          if (parseUnsignedValue(valueText, &intValue)) {
            settings_.batteryCheckIntervalMs =
                clampValue(intValue, 1000UL, 3600000UL);
            battery_.setIntervalMs(settings_.batteryCheckIntervalMs);
            updated = true;
          }
          break;
        default:
          break;
      }

      if (!updated) {
        CFG_SERIAL_PORT.println(F("Invalid value."));
        continue;
      }

      if (settingsStore_.save(settings_)) {
        CFG_SERIAL_PORT.println(F("Saved to EEPROM."));
      } else {
        CFG_SERIAL_PORT.println(F("Failed to save EEPROM settings."));
      }
    }
  }

  void menuRecordSampleAndSend() {
    if (!storage_.littleReady()) {
      CFG_SERIAL_PORT.println(F("LittleFS unavailable."));
      return;
    }

    CFG_SERIAL_PORT.print(F("Recording sample for "));
    CFG_SERIAL_PORT.print(CFG_SAMPLE_RECORD_MS);
    CFG_SERIAL_PORT.println(F(" ms..."));

    led_.setMode(LedMode::Recording);
    if (!recorder_.start(settings_)) {
      CFG_SERIAL_PORT.println(F("Could not start sample recorder."));
      led_.setMode(LedMode::Menu);
      return;
    }

    const uint32_t startMs = millis();
    while (millis() - startMs < CFG_SAMPLE_RECORD_MS) {
      recorder_.serviceCapture();
      if (!recorder_.isRunning()) {
        break;
      }
      serviceMenuHousekeeping();
    }
    recorder_.stop();
    led_.setMode(LedMode::Menu);

    if (recorder_.buffer().size() == 0) {
      CFG_SERIAL_PORT.println(F("Sample buffer is empty."));
      return;
    }

    String error;
    const String samplePath = "/sample.mp3";
    if (!storage_.saveBufferToExactPath(FsChoice::LittleFs, recorder_.buffer(),
                                        samplePath, &error)) {
      CFG_SERIAL_PORT.println(error);
      return;
    }

    CFG_SERIAL_PORT.print(F("Saved sample to "));
    CFG_SERIAL_PORT.println(samplePath);
    sendFileViaXmodem(FsChoice::LittleFs, samplePath);
  }

  void menuSendLittleFsFile() {
    if (!storage_.littleReady()) {
      CFG_SERIAL_PORT.println(F("LittleFS unavailable."));
      return;
    }
    CFG_SERIAL_PORT.print(F("LittleFS file path: "));
    const String path = readLineBlocking();
    if (path.length() == 0) {
      CFG_SERIAL_PORT.println(F("Empty path."));
      return;
    }
    sendFileViaXmodem(FsChoice::LittleFs, path);
  }

  void menuExitAndReboot() {
    CFG_SERIAL_PORT.print(F("Rebooting in "));
    CFG_SERIAL_PORT.print(CFG_MENU_EXIT_REBOOT_MS);
    CFG_SERIAL_PORT.println(F(" ms."));

    const uint32_t start = millis();
    while (millis() - start < CFG_MENU_EXIT_REBOOT_MS) {
      serviceMenuHousekeeping();
    }
    rp2040.reboot();
  }

  bool sendFileViaXmodem(FsChoice choice, const String &path) {
    fs::FS *fs = storage_.getFs(choice);
    if (!fs) {
      CFG_SERIAL_PORT.println(F("Storage unavailable."));
      return false;
    }
    if (!fs->exists(path.c_str())) {
      CFG_SERIAL_PORT.println(F("File does not exist."));
      return false;
    }

    if (!myXModem.begin(&CFG_SERIAL_PORT, XModem::CRC_XMODEM, *fs)) {
      CFG_SERIAL_PORT.println(F("XModem init failed."));
      return false;
    }

    CFG_SERIAL_PORT.print(F("Sending "));
    CFG_SERIAL_PORT.print(path);
    CFG_SERIAL_PORT.print(F(" from "));
    CFG_SERIAL_PORT.println(storage_.name(choice));
    CFG_SERIAL_PORT.println(F("Start an XMODEM/CRC receive on this serial link now."));
    CFG_SERIAL_PORT.flush();

    const bool ok = myXModem.sendFile(path);
    CFG_SERIAL_PORT.println();
    CFG_SERIAL_PORT.println(ok ? F("XModem transfer complete.")
                              : F("XModem transfer failed."));
    return ok;
  }

  bool promptFsChoice(FsChoice *choice) {
    if (!choice) {
      return false;
    }

    CFG_SERIAL_PORT.println(F("Choose storage:"));
    CFG_SERIAL_PORT.print(F("  1 - SD/SDFS"));
    if (!storage_.sdReady()) {
      CFG_SERIAL_PORT.print(F(" (unavailable)"));
    }
    CFG_SERIAL_PORT.println();

    CFG_SERIAL_PORT.print(F("  2 - LittleFS"));
    if (!storage_.littleReady()) {
      CFG_SERIAL_PORT.print(F(" (unavailable)"));
    }
    CFG_SERIAL_PORT.println();
    CFG_SERIAL_PORT.print(F("> "));

    const String selected = readLineBlocking();
    if (selected == "1") {
      *choice = FsChoice::Sd;
      return storage_.sdReady();
    }
    if (selected == "2") {
      *choice = FsChoice::LittleFs;
      return storage_.littleReady();
    }
    return false;
  }

  String readLineBlocking() {
    String line;
    while (true) {
      while (CFG_SERIAL_PORT.available()) {
        const char ch = static_cast<char>(CFG_SERIAL_PORT.read());
        if (ch == '\r' || ch == '\n') {
          if (line.length() > 0) {
            line.trim();
            return line;
          }
        } else if (ch >= ' ') {
          line += ch;
        }
      }
      serviceMenuHousekeeping();
      delay(1);
    }
  }

  void serviceMenuHousekeeping() {
    led_.update();
    const uint32_t nowMs = millis();
    serviceButton(nowMs);
    checkBattery(nowMs);
  }

  bool inMenu_ = false;
  bool serialUnlocked_ = false;

  volatile bool buttonRawPressed_ = false;
  volatile uint32_t buttonLastEdgeMs_ = 0;
  bool buttonWasDown_ = false;
  uint32_t buttonDownAtMs_ = 0;
  bool longPressHandled_ = false;

  uint16_t lastBatteryMv_ = 0;
  size_t largestAllocAtBoot_ = 0;
  size_t mp3RingCapacityAtBoot_ = 0;

  DeviceSettingsStore settingsStore_;
  DeviceSettings settings_ = DeviceSettingsStore::defaults();

  RgbLed led_;
  BatteryChecker battery_;
  StorageManager storage_;
  CircularRecorder recorder_;
};

bool onXmodemUpdateForwarder(uint8_t code, uint8_t value) {
  if (!gApp) {
    return true;
  }
  return gApp->onXmodemUpdate(code, value);
}

App app;

}  // namespace

void setup() { app.setup(); }

void loop() { app.loop(); }

void setup1() { app.setupCore1(); }

void loop1() { app.loopCore1(); }
