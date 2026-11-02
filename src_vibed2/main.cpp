#include <Arduino.h>
#include <I2S.h>
#include <Wire.h>
#include <XModem.h>
#include <hardware/irq.h>
#include <hardware/structs/scb.h>
#include <hardware/structs/systick.h>
#include <pico/multicore.h>
#include <pico/sync.h>

#include <ctype.h>
#include <stdlib.h>

#include "DeviceSettings.h"
#include "Hardware.h"
#include "LameEncoder.h"
#include "RecordingStore.h"
#include "VoiceCompressor.h"

bool core1_separate_stack = true;

namespace {
auto &console = CFG_SERIAL_PORT;
I2S microphone(INPUT);
PcmRing ramPcmRing;
Mp3FrameRing ramMp3Ring;
LameEncoder encoder;
VoiceCompressor compressor;
DeviceSettings settings;
DeviceSettingsStore settingsStore;
RecordingStore storage;
RgbLed led;
UserButton button;
BatteryChecker battery;

semaphore_t workerStart;
semaphore_t workerReady;
semaphore_t workerDone;
std::atomic<bool> syncReady{false};
std::atomic<bool> producerDone{false};
std::atomic<bool> encoderFailed{false};
enum class WorkerCommand { Prepare, Record };
WorkerCommand workerCommand = WorkerCommand::Prepare;
bool encoderPrepared = false; // Core 1 only.

bool serialEnabled = false;
bool recording = false;
bool microphoneRunning = false;
bool inMenu = false;
bool lowBattery = false;
bool settingsReady = false;
bool usedDefaults = false;
bool unsaved = false;
uint64_t capturedSamples = 0;
const char *captureError = nullptr;
uint8_t *mp3Memory = nullptr;
uint32_t stopLagMs = 0;

enum class MenuPage {
  Main, ListFs, SendFs, SendPath, EraseFs, EraseConfirm,
  CopyFolder, Quality, SampleConfirm, FormatConfirm
};
MenuPage page = MenuPage::Main;
StorageKind selectedFs = StorageKind::LittleFs;
char line[CFG_MENU_LINE_BYTES];
size_t lineSize = 0;
bool lineTooLong = false;
bool lastWasCr = false;

void showStatusAndMenu();
void service();
void enterMenu(const char *error = nullptr);
[[noreturn]] void powerOff();

bool powerRequested() {
  return lowBattery || button.longPress();
}

void enableSerial() {
  if (!serialEnabled) {
    console.begin(CFG_SERIAL_BAUD);
    serialEnabled = true;
  }
}

void service() {
  led.tick();
  button.tick();
  if (battery.tick() &&
      battery.millivolts() < CFG_BATTERY_LOW_THRESHOLD_MV) {
    lowBattery = true;
  }
}

bool storageService() {
  service();
  return !powerRequested();
}

[[noreturn]] void workerTimeout() {
  enableSerial();
  console.println("FATAL: encoder worker timed out. RAM retained; reset required.");
  led.set(LedState::Error);
  while (true) {
    service();
    if (powerRequested()) {
      // Do not touch a ring whose worker has not acknowledged stopping.
      recording = false;
      unsaved = false;
      powerOff();
    }
    delay(1);
  }
}

void waitFor(semaphore_t &semaphore) {
  const uint32_t started = millis();
  while (!sem_try_acquire(&semaphore)) {
    service();
    if (millis() - started >= CFG_ENCODER_TIMEOUT_MS) {
      workerTimeout();
    }
    delay(1);
  }
}

bool configureMicrophone() {
  static_assert(CFG_I2S_LRCLK_PIN == CFG_I2S_BCLK_PIN + 1,
                "Arduino-Pico I2S requires LRCLK = BCLK + 1");
  return microphone.setBCLK(CFG_I2S_BCLK_PIN) &&
         microphone.setDATA(CFG_I2S_DATA_PIN) &&
         microphone.setBitsPerSample(32) &&
         microphone.setFrequency(CFG_AUDIO_SAMPLE_RATE) &&
         microphone.setBuffers(CFG_MIC_BUFFER_COUNT, CFG_MIC_BUFFER_WORDS);
}

bool allocateMp3Ring() {
  void *reserve = malloc(CFG_RUNTIME_RESERVE_BYTES);
  if (!reserve) {
    return false;
  }
  size_t low = 0;
  size_t high = rp2040.getFreeHeap() / kMp3FrameBytes;
  while (low < high) {
    const size_t frames = low + (high - low + 1) / 2;
    void *probe = malloc(frames * kMp3FrameBytes);
    if (probe) {
      free(probe);
      low = frames;
    } else {
      high = frames - 1;
    }
  }
  if (low >= CFG_MIN_MP3_FRAMES) {
    mp3Memory = static_cast<uint8_t *>(malloc(low * kMp3FrameBytes));
  }
  free(reserve);
  if (!mp3Memory) {
    return false;
  }
  ramMp3Ring.attach(mp3Memory, low * kMp3FrameBytes);
  return true;
}

bool startRecording() {
  if (!mp3Memory || powerRequested()) {
    return false;
  }
  if (!microphoneRunning) {
    microphoneRunning = microphone.begin();
    if (!microphoneRunning) {
      captureError = "I2S start failed.";
      return false;
    }
  }
  ramPcmRing.reset();
  ramMp3Ring.reset();
  capturedSamples = 0;
  captureError = nullptr;
  compressor.begin(settings.audio);
  producerDone.store(false, std::memory_order_relaxed);
  encoderFailed.store(false, std::memory_order_relaxed);
  workerCommand = WorkerCommand::Record;
  sem_release(&workerStart);
  waitFor(workerReady);
  if (encoderFailed.load(std::memory_order_acquire)) {
    waitFor(workerDone);
    microphone.end();
    microphoneRunning = false;
    captureError = "LAME initialization failed.";
    return false;
  }
  // Encoder startup/heap probing are not part of this recording.
  while (microphone.available() >= 8) {
    int32_t left, right;
    if (!microphone.read32(&left, &right)) {
      producerDone.store(true, std::memory_order_release);
      waitFor(workerDone);
      microphone.end();
      microphoneRunning = false;
      captureError = "I2S drain failed.";
      return false;
    }
  }
  microphone.getOverflow();
  recording = true;
  unsaved = true;
  led.set(LedState::Recording);
  return true;
}

bool captureBatch(uint64_t sampleLimit = UINT64_MAX) {
  for (size_t i = 0; i < CFG_PCM_BLOCK_SAMPLES &&
                     capturedSamples < sampleLimit; ++i) {
    int32_t left, right;
    if (!microphone.read32(&left, &right)) {
      captureError = "I2S read failed.";
      return false;
    }
    const int32_t sample = CFG_I2S_USE_RIGHT_CHANNEL ? right : left;
    if (!ramPcmRing.push(compressor.process(sample))) {
      captureError = "PCM ring overrun: encoder cannot keep up. Audio incomplete.";
      return false;
    }
    ++capturedSamples;
  }
  if (encoderFailed.load(std::memory_order_acquire)) {
    captureError = "MP3 encoding failed. Complete frames retained.";
    return false;
  }
  if (microphone.getOverflow()) {
    captureError = "I2S overflow: samples lost. Complete MP3 frames retained.";
    return false;
  }
  return true;
}

void stopRecording() {
  if (!recording) {
    return;
  }
  if (microphone.getOverflow()) {
    captureError = "I2S overflow: samples lost.";
  }
  microphone.end();
  microphoneRunning = false;
  const uint32_t stopped = millis();
  producerDone.store(true, std::memory_order_release);
  waitFor(workerDone);
  stopLagMs = millis() - stopped;
  recording = false;
  if (encoderFailed.load(std::memory_order_acquire)) {
    captureError = "MP3 encode/flush/reinitialization failed. Complete frames retained.";
  }
}

bool saveSnapshot() {
  led.set(LedState::Saving);
  if (!ramMp3Ring.size()) {
    if (serialEnabled) {
      console.println("ERROR: no complete MP3 frames to save.");
    }
    return false;
  }
  const bool saved = storage.save(ramMp3Ring);
  if (saved) {
    unsaved = false;
    if (serialEnabled) {
      console.printf("Saved and verified %s:%s (%lu bytes).\r\n",
                     storage.name(storage.lastKind()), storage.lastPath(),
                     static_cast<unsigned long>(ramMp3Ring.size()));
    }
  }
  return saved;
}

void enterMenu(const char *error) {
  stopRecording();
  enableSerial();
  inMenu = true;
  page = MenuPage::Main;
  led.set(error || captureError ? LedState::Error : LedState::Menu);
  if (error) {
    console.printf("ERROR: %s\r\n", error);
  }
  if (captureError) {
    console.printf("ERROR: %s\r\n", captureError);
  }
  showStatusAndMenu();
}

void showQuality() {
  console.printf(
      "Audio quality (16 kHz mono / 24 kbps fixed):\r\n"
      "  1 high-pass Hz: %.3f [0..2000]\r\n"
      "  2 input gain:   %.3f [0.1..40]\r\n"
      "  3 makeup gain:  %.3f [0.1..40]\r\n"
      "  4 ceiling:      %.4f [0.1..0.9999]\r\n"
      "  5 attack ms:    %.3f [1..2000]\r\n"
      "  6 release ms:   %.3f [1..5000]\r\n"
      "  7 threshold:    %.4f [0.0001..1]\r\n"
      "  8 ratio:        %.3f [1..50]\r\n",
      settings.audio.highPassHz, settings.audio.inputGain,
      settings.audio.makeupGain, settings.audio.ceiling,
      settings.audio.attackMs, settings.audio.releaseMs,
      settings.audio.threshold, settings.audio.ratio);
}

void showStatusAndMenu() {
  console.println("\r\n--- PastRecorder: circular MP3 / maintenance ---");
  console.printf("State: paused; reset reason=%u; EEPROM=%s%s\r\n",
                 static_cast<unsigned>(rp2040.getResetReason()),
                 settingsReady ? "OK" : "ERROR",
                 usedDefaults ? " (defaults restored at boot)" : "");
  showQuality();
  console.printf(
      "Battery=%u mV; low=%u; recharge=%u; interval=%lu ms; low-off flag=%u\r\n"
      "PCM: %u samples / %lu bytes; peak=%lu; encoder stop lag=%lu ms\r\n"
      "MP3: %lu/%lu bytes, %lu/%lu frames; retained=%lu ms; capacity=%lu ms\r\n"
      "Captured=%llu samples; total encoded=%llu frames; snapshot=%s\r\n"
      "Free heap=%lu bytes; runtime reserve=%u bytes; LAME quality=%u\r\n",
      battery.millivolts(), CFG_BATTERY_LOW_THRESHOLD_MV,
      CFG_BATTERY_LOW_THRESHOLD_MV + CFG_BATTERY_RECHARGE_DELTA_MV,
      static_cast<unsigned long>(CFG_BATTERY_CHECK_INTERVAL_MS),
      settings.lowBatteryShutdown, CFG_PCM_RING_SAMPLES,
      static_cast<unsigned long>(CFG_PCM_RING_SAMPLES * sizeof(int16_t)),
      static_cast<unsigned long>(ramPcmRing.peak()),
      static_cast<unsigned long>(stopLagMs),
      static_cast<unsigned long>(ramMp3Ring.size()),
      static_cast<unsigned long>(ramMp3Ring.capacity()),
      static_cast<unsigned long>(ramMp3Ring.size() / kMp3FrameBytes),
      static_cast<unsigned long>(ramMp3Ring.capacity() / kMp3FrameBytes),
      static_cast<unsigned long>(ramMp3Ring.durationMs()),
      static_cast<unsigned long>(ramMp3Ring.capacity() / kMp3FrameBytes *
                                 kMp3FrameSamples * 1000 / CFG_AUDIO_SAMPLE_RATE),
      static_cast<unsigned long long>(capturedSamples),
      static_cast<unsigned long long>(ramMp3Ring.framesWritten()),
      unsaved ? "UNSAVED (kept in RAM)" : "saved / empty",
      static_cast<unsigned long>(rp2040.getFreeHeap()),
      CFG_RUNTIME_RESERVE_BYTES, CFG_MP3_LAME_QUALITY);
  console.printf(
      "Pins: I2S BCLK=%u LRCLK=%u DATA=%u right=%u; button=%u low=%u hold=%u ms\r\n"
      "SD CS=%u SCK=%u MISO=%u MOSI=%u Hz=%u; RGB=%u/%u/%u inverted=%u\r\n"
      "Battery ADC GPIO=%u channel=%u scale=%u offset=%d mV; reboot delay=%u ms\r\n"
      "Serial baud=%u USB-detect=%u; I2S DMA=%u x %u words\r\n",
      CFG_I2S_BCLK_PIN, CFG_I2S_LRCLK_PIN, CFG_I2S_DATA_PIN,
      CFG_I2S_USE_RIGHT_CHANNEL, CFG_BUTTON_PIN, CFG_BUTTON_ACTIVE_LOW,
      CFG_LONG_PRESS_POWER_OFF_MS, CFG_SD_CS_PIN, CFG_SD_SCK_PIN,
      CFG_SD_MISO_PIN, CFG_SD_MOSI_PIN, CFG_SD_SPI_HZ, CFG_LED_R_PIN,
      CFG_LED_G_PIN, CFG_LED_B_PIN, CFG_LED_INVERT_LOGIC,
      CFG_BATTERY_ADC_GPIO, CFG_BATTERY_ADC_INPUT, CFG_BATTERY_SCALE_MV,
      CFG_BATTERY_OFFSET_MV, CFG_MENU_EXIT_REBOOT_MS,
      CFG_SERIAL_BAUD, CFG_SERIAL_USB, CFG_MIC_BUFFER_COUNT, CFG_MIC_BUFFER_WORDS);
  storage.report(console);
  console.println(
      "  1 - List files (choose LittleFS / SD)\r\n"
      "  2 - Send file by XMODEM/CRC (choose LittleFS / SD)\r\n"
      "  3 - Erase all files (choose LittleFS / SD; confirmation required)\r\n"
      "  4 - Copy ALL LittleFS files to a NEW SD folder\r\n"
      "  5 - Audio quality settings (save to EEPROM)\r\n"
      "  6 - Record 5-second MP3 sample, save, send by XMODEM\r\n"
      "  7 - Send LittleFS file by XMODEM/CRC\r\n"
      "  8 - Exit, sleep configured interval, reboot\r\n"
      "  9 - Save/retry retained RAM snapshot (SD then LittleFS)\r\n"
      "  F - Initialize/format LittleFS (confirmation required)\r\n"
      "Enter option followed by Enter:");
}

// Keep LEDs and power protection alive even during XMODEM's blocking reads.
class MaintenanceStream : public Stream {
 public:
  int available() override { maintain(); return console.available(); }
  int read() override { maintain(); return console.read(); }
  int peek() override { maintain(); return console.peek(); }
  void flush() override { maintain(); console.flush(); }
  size_t write(uint8_t byte) override { maintain(); return console.write(byte); }
  size_t write(const uint8_t *bytes, size_t count) override {
    maintain();
    return console.write(bytes, count);
  }

 private:
  void maintain() {
    service();
    if (powerRequested()) {
      powerOff();
    }
  }
};

bool xmodemUpdate(uint8_t code, uint8_t value) {
  service();
  // No text output on the protocol stream, including error callbacks.
  return !powerRequested() && code != 2 &&
         !(code == 1 && (value == 1 || value == 2));
}

void sendFile(StorageKind kind, const char *path) {
  if (!storage.mounted(kind)) {
    console.println("ERROR: selected filesystem is not mounted.");
    return;
  }
  fs::File file = storage.filesystem(kind).open(path, "r");
  if (!file || !file.isFile()) {
    console.println("ERROR: file not found or not a regular file.");
    return;
  }
  console.printf("Sending %s:%s, exactly %lu bytes.\r\n",
                 storage.name(kind), path,
                 static_cast<unsigned long>(file.size()));
  file.close();
  console.println("Start XMODEM/CRC receive now (strip protocol padding to size).");
  console.flush();
  MaintenanceStream stream;
  stream.setTimeout(100);
  XModem transfer;
  transfer.onXmodemUpdate(xmodemUpdate);
  led.set(LedState::Saving);
  const bool ok =
      transfer.begin(&stream, XModem::CRC_XMODEM, storage.filesystem(kind)) &&
      transfer.sendFile(path);
  console.println(ok ? "\r\nXMODEM complete." : "\r\nERROR: XMODEM failed/cancelled.");
  led.set(ok ? LedState::Menu : LedState::Error);
}

bool chooseFs(const char *text, StorageKind &kind) {
  if (strcmp(text, "1") == 0) {
    kind = StorageKind::LittleFs;
    return true;
  }
  if (strcmp(text, "2") == 0) {
    kind = StorageKind::Sd;
    return true;
  }
  return false;
}

void sampleAndSend() {
  if (!startRecording()) {
    enterMenu(captureError ? captureError : "Sample could not start.");
    return;
  }
  const uint64_t count =
      static_cast<uint64_t>(CFG_AUDIO_SAMPLE_RATE) * CFG_SAMPLE_RECORD_MS / 1000;
  console.printf("Recording %lu ms sample...\r\n",
                 static_cast<unsigned long>(CFG_SAMPLE_RECORD_MS));
  while (capturedSamples < count && !powerRequested()) {
    service();
    if (!captureBatch(count)) {
      break;
    }
  }
  stopRecording();
  if (powerRequested()) {
    powerOff();
  }
  if (captureError || !saveSnapshot()) {
    enterMenu(captureError ? captureError : "Sample save failed; RAM retained.");
    return;
  }
  sendFile(storage.lastKind(), storage.lastPath());
}

void processLine(char *text) {
  while (*text && isspace(static_cast<unsigned char>(*text))) {
    ++text;
  }
  size_t size = strlen(text);
  while (size && isspace(static_cast<unsigned char>(text[size - 1]))) {
    text[--size] = 0;
  }
  if (page == MenuPage::ListFs || page == MenuPage::SendFs ||
      page == MenuPage::EraseFs) {
    if (!chooseFs(text, selectedFs)) {
      console.println("ERROR: choose 1 (LittleFS) or 2 (SD).");
    } else if (page == MenuPage::ListFs) {
      storage.list(selectedFs, console);
    } else if (page == MenuPage::SendFs) {
      page = MenuPage::SendPath;
      console.println("Enter absolute file path:");
      return;
    } else {
      page = MenuPage::EraseConfirm;
      console.printf("Type ERASE to delete ALL %s files (not EEPROM):\r\n",
                     storage.name(selectedFs));
      return;
    }
  } else if (page == MenuPage::SendPath) {
    if (*text == '/') {
      sendFile(selectedFs, text);
    } else {
      console.println("ERROR: absolute path required.");
    }
  } else if (page == MenuPage::EraseConfirm) {
    if (strcmp(text, "ERASE") == 0) {
      led.set(LedState::Saving);
      const bool ok = storage.erase(selectedFs, console);
      console.println(ok ? "All files erased." : "ERROR: erase incomplete.");
      led.set(ok ? LedState::Menu : LedState::Error);
    } else {
      console.println("Erase cancelled.");
    }
  } else if (page == MenuPage::CopyFolder) {
    char folder[48];
    bool valid = size <= 32;
    for (size_t i = 0; i < size; ++i) {
      valid = valid && (isalnum(static_cast<unsigned char>(text[i])) ||
                        text[i] == '_' || text[i] == '-');
    }
    if (!valid) {
      console.println("ERROR: folder must be 1..32 letters/digits/_/- or empty.");
    } else {
      if (size) {
        snprintf(folder, sizeof(folder), "/%s", text);
      } else {
        do {
          snprintf(folder, sizeof(folder), "/BACKUP_%08lx",
                   static_cast<unsigned long>(rp2040.hwrand32()));
        } while (storage.filesystem(StorageKind::Sd).exists(folder));
      }
      led.set(LedState::Saving);
      console.printf("Copy destination: %s\r\n", folder);
      const bool ok = storage.copyToSd(folder, console);
      led.set(ok ? LedState::Menu : LedState::Error);
    }
  } else if (page == MenuPage::Quality) {
    if (strcmp(text, "0") != 0) {
      unsigned index;
      float value;
      char extra;
      DeviceSettings candidate = settings;
      float *fields[] = {
          &candidate.audio.highPassHz, &candidate.audio.inputGain,
          &candidate.audio.makeupGain, &candidate.audio.ceiling,
          &candidate.audio.attackMs, &candidate.audio.releaseMs,
          &candidate.audio.threshold, &candidate.audio.ratio};
      if (text[0] < '1' || text[0] > '8' ||
          !isspace(static_cast<unsigned char>(text[1])) ||
          sscanf(text, "%1u %f %c", &index, &value, &extra) != 2 ||
          index < 1 || index > 8 || !isfinite(value)) {
        console.println("ERROR: use 'setting-number value'; finite numbers only.");
      } else {
        *fields[index - 1] = value;
        if (!candidate.audio.valid()) {
          console.println("ERROR: outside documented setting range.");
        } else if (!settingsReady || !settingsStore.save(candidate)) {
          console.println("ERROR: EEPROM commit failed; settings not applied.");
        } else {
          settings = candidate;
          console.println("Settings saved to EEPROM.");
        }
      }
      showQuality();
      console.println("Enter 'number value' to save, or 0 to return:");
      return;
    }
  } else if (page == MenuPage::SampleConfirm) {
    if (strcmp(text, "SAMPLE") == 0) {
      sampleAndSend();
    } else {
      console.println("Sample cancelled; retained audio unchanged.");
    }
  } else if (page == MenuPage::FormatConfirm) {
    if (strcmp(text, "FORMAT") == 0) {
      led.set(LedState::Saving);
      const bool ok = storage.formatLittleFs();
      console.println(ok ? "LittleFS formatted and mounted." :
                           "ERROR: LittleFS format/mount failed.");
      led.set(ok ? LedState::Menu : LedState::Error);
    } else {
      console.println("Format cancelled.");
    }
  } else if (strcmp(text, "1") == 0 || strcmp(text, "2") == 0 ||
             strcmp(text, "3") == 0) {
    page = *text == '1' ? MenuPage::ListFs :
           *text == '2' ? MenuPage::SendFs : MenuPage::EraseFs;
    storage.begin();
    console.println("Choose 1 - LittleFS | 2 - SD:");
    return;
  } else if (strcmp(text, "4") == 0) {
    page = MenuPage::CopyFolder;
    console.println("Enter NEW folder name, or Enter to randomize:");
    return;
  } else if (strcmp(text, "5") == 0) {
    page = MenuPage::Quality;
    showQuality();
    console.println("Enter 'number value' to save, or 0 to return:");
    return;
  } else if (strcmp(text, "6") == 0) {
    page = MenuPage::SampleConfirm;
    console.println("Type SAMPLE to replace retained RAM audio with a new sample:");
    return;
  } else if (strcmp(text, "7") == 0) {
    page = MenuPage::SendPath;
    selectedFs = StorageKind::LittleFs;
    console.println("Enter absolute LittleFS file path:");
    return;
  } else if (strcmp(text, "8") == 0) {
    console.printf("Reboot in %u ms. %s\r\n", CFG_MENU_EXIT_REBOOT_MS,
                   unsaved ? "WARNING: UNSAVED RAM WILL BE LOST (use option 9)." : "");
    console.flush();
    const uint32_t started = millis();
    while (millis() - started < CFG_MENU_EXIT_REBOOT_MS) {
      service();
      if (powerRequested()) {
        powerOff();
      }
      delay(1);
    }
    storage.end();
    rp2040.reboot();
  } else if (strcmp(text, "9") == 0) {
    const bool saved = saveSnapshot();
    console.println(saved ? "Snapshot saved." :
                            "ERROR: save failed; snapshot retained in RAM.");
    led.set(saved ? LedState::Menu : LedState::Error);
  } else if (strcmp(text, "F") == 0 || strcmp(text, "f") == 0) {
    page = MenuPage::FormatConfirm;
    console.println("Type FORMAT to destroy/reinitialize LittleFS (not EEPROM):");
    return;
  } else if (size) {
    console.println("ERROR: invalid option.");
  }
  page = MenuPage::Main;
  showStatusAndMenu();
}

void readMenu() {
  while (console.available()) {
    const int value = console.read();
    if (value < 0) {
      break;
    }
    const char c = static_cast<char>(value);
    if (c == '\n' && lastWasCr) {
      lastWasCr = false;
      continue;
    }
    lastWasCr = c == '\r';
    if (c == '\r' || c == '\n') {
      line[lineSize] = 0;
      const bool overflow = lineTooLong;
      lineSize = 0;
      lineTooLong = false;
      if (overflow) {
        console.println("ERROR: input too long.");
        page = MenuPage::Main;
        showStatusAndMenu();
      } else {
        processLine(line);
      }
    } else if ((c == '\b' || c == 127) && lineSize && !lineTooLong) {
      --lineSize;
    } else if (c >= 32 && c <= 126) {
      if (lineSize + 1 < sizeof(line)) {
        line[lineSize++] = c;
      } else {
        lineTooLong = true;
      }
    } else {
      lineTooLong = true;
    }
    service();
    if (powerRequested()) {
      powerOff();
    }
  }
}

[[noreturn]] void powerOff() {
  stopRecording();
  if (lowBattery && !settings.lowBatteryShutdown) {
    settings.lowBatteryShutdown = 1;
    if (!settingsReady || !settingsStore.save(settings)) {
      enableSerial();
      console.println("ERROR: could not persist low-battery shutdown flag.");
    }
  }
  if (unsaved && ramMp3Ring.size() && !saveSnapshot()) {
    enableSerial();
    console.println("ERROR: shutdown save failed; unsaved RAM will be lost.");
    storage.report(console);
  }
  if (serialEnabled) {
    console.println(lowBattery ? "Low battery: software halt; recharge then reset." :
                                "Manual software halt; reset to restart.");
    console.flush();
  }
  led.set(lowBattery ? LedState::LowBattery : LedState::Boot);
  const uint32_t started = millis();
  while (millis() - started < CFG_LOW_BATTERY_SIGNAL_MS) {
    led.tick();
    delay(1);
  }
  if (microphoneRunning) {
    microphone.end();
  }
  storage.end();
  Wire.end();
  console.end();
  led.set(LedState::Off);
  multicore_reset_core1();
  save_and_disable_interrupts();
  systick_hw->csr = 0;
  for (uint irq = 0; irq < NUM_IRQS; ++irq) {
    irq_set_enabled(irq, false);
    irq_clear(irq);
  }
  // Pending system exceptions can otherwise make masked WFI spin.
#if PICO_RP2040
  scb_hw->icsr = M0PLUS_ICSR_PENDSTCLR_BITS | M0PLUS_ICSR_PENDSVCLR_BITS;
#else
  scb_hw->icsr = M33_ICSR_PENDSTCLR_BITS | M33_ICSR_PENDSVCLR_BITS;
#endif
  __dsb();
  while (true) {
    __wfi();
  }
}
}

void setup1() {
  while (!syncReady.load(std::memory_order_acquire)) {
    delay(1);
  }
}

void loop1() {
  sem_acquire_blocking(&workerStart);
  if (workerCommand == WorkerCommand::Prepare) {
    const bool ok = encoder.warmUp() && encoder.begin(ramMp3Ring);
    encoderPrepared = ok;
    encoderFailed.store(!ok, std::memory_order_release);
    sem_release(&workerDone);
    return;
  }
  const bool started = encoderPrepared || encoder.begin(ramMp3Ring);
  encoderPrepared = false;
  encoderFailed.store(!started, std::memory_order_release);
  sem_release(&workerReady);
  bool ok = started;
  int16_t block[CFG_PCM_BLOCK_SAMPLES];
  size_t staged = 0;
  while (ok) {
    // Load completion before the head: seeing done must also see the last PCM.
    const bool done = producerDone.load(std::memory_order_acquire);
    const size_t count =
        ramPcmRing.pop(block + staged, CFG_PCM_BLOCK_SAMPLES - staged);
    staged += count;
    if (staged == CFG_PCM_BLOCK_SAMPLES || (done && !count)) {
      if (staged) {
        ok = encoder.write(block, staged);
        staged = 0;
      }
    }
    if (!count) {
      if (done) {
        break;
      }
      delay(1);
      continue;
    }
  }
  if (started) {
    ok = encoder.finish() && ok;
    // Reclaim LAME's large heap blocks BEFORE storage can fragment those holes.
    encoderPrepared = encoder.begin(ramMp3Ring);
    ok = encoderPrepared && ok;
  }
  encoderFailed.store(!ok, std::memory_order_release);
  sem_release(&workerDone);
}

void setup() {
  led.begin();
  button.begin();
  battery.begin();
#if !CFG_SERIAL_USB
  // Listening is required to detect UART activity; no boot output is sent.
  console.begin(CFG_SERIAL_BAUD);
#endif
  settingsReady = settingsStore.begin(settings, usedDefaults);
  const uint16_t restartThreshold =
      CFG_BATTERY_LOW_THRESHOLD_MV +
      (settingsReady && settings.lowBatteryShutdown ?
           CFG_BATTERY_RECHARGE_DELTA_MV : 0);
  if (battery.millivolts() < restartThreshold) {
    lowBattery = true;
    powerOff();
  }
  if (settingsReady) {
    if (settings.lowBatteryShutdown) {
      settings.lowBatteryShutdown = 0;
      settingsReady = settingsStore.save(settings);
    }
  }
  sem_init(&workerStart, 0, 1);
  sem_init(&workerReady, 0, 1);
  sem_init(&workerDone, 0, 1);
  syncReady.store(true, std::memory_order_release);
  storage.setService(storageService);
  storage.begin();
  if (!settingsReady) {
    enterMenu("EEPROM initialization/commit failed. Recording disabled.");
    return;
  }
  if (!configureMicrophone() || !microphone.begin()) {
    enterMenu("Could not configure/start I2S.");
    return;
  }
  microphoneRunning = true;
  workerCommand = WorkerCommand::Prepare;
  sem_release(&workerStart);
  waitFor(workerDone);
  if (encoderFailed.load(std::memory_order_acquire) || !allocateMp3Ring()) {
    microphone.end();
    microphoneRunning = false;
    enterMenu("LAME warm-up or MP3 RAM allocation failed.");
    return;
  }
  if (!storage.available()) {
    microphone.end();
    microphoneRunning = false;
    enterMenu("No writable storage. RAM allocated; insert SD or initialize LittleFS.");
    return;
  }
  if (!startRecording()) {
    enterMenu(captureError ? captureError : "Recording could not start.");
  }
}

void loop() {
  service();
  if (powerRequested()) {
    powerOff();
  }
  bool serialActivity = console.available() > 0;
#if CFG_SERIAL_USB
  serialActivity = serialActivity || static_cast<bool>(console);
#endif
  if (!inMenu && serialActivity) {
    enterMenu();
  }
  if (inMenu) {
    button.takeShortPress();
    readMenu();
    delay(1);
    return;
  }
  if (button.takeShortPress()) {
    stopRecording();
    if (captureError) {
      enterMenu();
    } else if (!saveSnapshot()) {
      enterMenu("Both storage targets failed; RAM snapshot retained. Use option 9.");
    } else if (!powerRequested() && !startRecording()) {
      enterMenu(captureError ? captureError : "Recording restart failed.");
    }
    return;
  }
  if (recording && !captureBatch()) {
    enterMenu();
  }
}
