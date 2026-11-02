#ifdef PASTRECORDER_STORAGE_TEST

#include <assert.h>
#include <fstream>

#include "../RecordingStore.h"

namespace {
unsigned serviceCalls = 0;
bool cancelOperation = false;
bool service() { ++serviceCalls; return !cancelOperation; }

void checkFile(const RecordingStore &storage, const Mp3FrameRing &ring) {
  fs::FS &fs = storage.lastKind() == StorageKind::Sd ?
                   SDFS : static_cast<fs::FS &>(LittleFS);
  fs::File saved = fs.open(storage.lastPath(), "r");
  assert(saved && saved.size() == ring.size());
  for (size_t offset = 0; offset < ring.size();) {
    size_t count;
    const uint8_t *bytes = ring.segment(offset, count);
    std::vector<uint8_t> actual(count);
    assert(saved.read(actual.data(), count) == static_cast<int>(count));
    assert(memcmp(bytes, actual.data(), count) == 0);
    offset += count;
  }
}
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::filesystem::path root(argv[1]);
  // Only clean a test directory created from scratch by this invocation.
  assert(std::filesystem::create_directory(root));
  SDFS.root = root / "sd";
  LittleFS.root = root / "flash";
  std::filesystem::create_directory(SDFS.root);
  std::filesystem::create_directory(LittleFS.root);
  RecordingStore storage;
  storage.setService(service);
  storage.begin();
  uint8_t memory[kMp3FrameBytes * 17];
  uint8_t frame[kMp3FrameBytes] = {0xFF, 0xF3, 0x38, 0xC4, 0};
  Mp3FrameRing ring;
  ring.attach(memory, sizeof(memory));
  for (unsigned i = 0; i < 53; ++i) {
    frame[8] = i;
    assert(ring.append(frame, sizeof(frame)));
  }
  assert(storage.save(ring));
  assert(storage.lastKind() == StorageKind::Sd);
  checkFile(storage, ring);
  const std::string first = storage.lastPath();
  assert(serviceCalls > 4);

  SDFS.capacity = 1;
  assert(storage.save(ring));
  assert(storage.lastKind() == StorageKind::LittleFs);
  checkFile(storage, ring);
  LittleFS.capacity = 1;
  assert(!storage.save(ring));
  assert(ring.framesWritten() == 53 && ring.size() == sizeof(memory));
  SDFS.capacity = LittleFS.capacity = 1u << 24;

  SDFS.failCreate = true;
  assert(storage.save(ring) && storage.lastKind() == StorageKind::LittleFs);
  SDFS.failCreate = false;
  SDFS.shortWrite = true;
  assert(storage.save(ring) && storage.lastKind() == StorageKind::LittleFs);
  SDFS.shortWrite = false;
  SDFS.corruptRead = true;
  assert(storage.save(ring) && storage.lastKind() == StorageKind::LittleFs);
  SDFS.corruptRead = false;
  SDFS.failRename = true;
  assert(storage.save(ring) && storage.lastKind() == StorageKind::LittleFs);
  SDFS.failRename = false;
  for (const auto &entry : std::filesystem::directory_iterator(SDFS.root)) {
    assert(entry.path().extension() != ".tmp");
  }
  SDFS.shortWrite = SDFS.failRemove = true;
  assert(storage.save(ring));
  assert(strstr(storage.status(StorageKind::Sd), "removal failed"));
  SDFS.shortWrite = SDFS.failRemove = false;

  RecordingStore afterReboot;
  afterReboot.begin();
  assert(afterReboot.save(ring));
  assert(afterReboot.lastKind() == StorageKind::Sd);
  assert(first != afterReboot.lastPath());
  checkFile(afterReboot, ring);

  Print output;
  assert(storage.list(StorageKind::Sd, output));
  assert(output.output.find(".mp3") != std::string::npos);
  assert(LittleFS.mkdir("/nested"));
  {
    std::ofstream file(LittleFS.root / "nested" / "sample.bin", std::ios::binary);
    file << "preserve me";
  }
  assert(storage.copyToSd("/backup", output));
  assert(SDFS.exists("/backup/nested/sample.bin"));
  assert(LittleFS.exists("/nested/sample.bin"));
  assert(!storage.copyToSd("/backup", output)); // Never reuse a backup folder.
  SDFS.corruptRead = true;
  assert(!storage.copyToSd("/badbackup", output));
  assert(!SDFS.exists("/badbackup/nested/sample.bin"));
  SDFS.corruptRead = false;
  cancelOperation = true;
  assert(!storage.copyToSd("/cancelled", output));
  assert(LittleFS.exists("/nested/sample.bin"));
  cancelOperation = false;
  assert(storage.erase(StorageKind::Sd, output));
  assert(std::filesystem::is_empty(SDFS.root));
  assert(storage.erase(StorageKind::LittleFs, output));
  assert(std::filesystem::is_empty(LittleFS.root));

  SD.mountOk = false;
  assert(storage.save(ring));
  assert(storage.lastKind() == StorageKind::LittleFs);
  SD.mountOk = true;
  assert(storage.save(ring));
  assert(storage.lastKind() == StorageKind::Sd); // A card can be inserted/swapped.
  SD.mountOk = false;
  assert(storage.erase(StorageKind::Sd, output));
  LittleFS.mountOk = false;
  storage.end();
  storage.begin();
  assert(!storage.available() && !storage.save(ring));
  assert(storage.formatLittleFs()); // Only explicit formatting initializes flash.
  assert(storage.available());
  assert(storage.erase(StorageKind::LittleFs, output));
  storage.end();
  assert(std::filesystem::remove(SDFS.root));
  assert(std::filesystem::remove(LittleFS.root));
  assert(std::filesystem::remove(root));
  puts("All storage fallback, verification, copy, erase and collision tests passed.");
}

#endif
