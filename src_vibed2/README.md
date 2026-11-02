# Circular audio recorder

The firmware starts recording without waiting for or printing to Serial. Core 0
captures 32-bit stereo I2S, selects the microphone channel, high-passes, applies
gain/compression/limiting, and publishes 16-bit mono PCM. Core 1 runs LAME at
**16 kHz / 24 kbps CBR**. The microphones are **I2S, not I2C**.

## Configuration and wiring

All defaults are in [config.h](config.h), with `#ifndef` guards for build-time
overrides. Default pins (verify against your board and wiring):

| Function | GPIO |
|---|---|
| Microphone BCLK / LRCLK / DATA | 6 / 7 / 10 |
| SD SCK / MOSI / MISO / CS, SPI | 2 / 3 / 4 / 5 |
| Discrete RGB red / green / blue | 11 / 12 / 13 |
| Button, active-low with pull-up | 14 |
| Battery ADC, **VSYS divided by 3** | 29, ADC3 |

RGB outputs default to active-low. These are three discrete LED outputs, **not**
the addressable LED fitted to some Zero boards. Use suitable current-limiting
resistors. Arduino-Pico fixes LRCLK at BCLK + 1.

Battery defaults: low at **3250 mV**, recovery at **3450 mV** after a low-battery
halt, checked every **15 seconds**. The GPIO29/VSYS divider is board-dependent:
verify it and calibrate `CFG_BATTERY_SCALE_MV` / `CFG_BATTERY_OFFSET_MV` before
relying on protection. Never connect undivided battery voltage to an ADC pin.
This firmware does not replace a cell protection/charging circuit.

`CFG_SERIAL_PORT` selects the stream; **also** set `CFG_SERIAL_USB` appropriately:

* `Serial`, `CFG_SERIAL_USB=1`: USB CDC DTR/host connection or received bytes opens
  maintenance. USB itself is initialized by the Arduino core; no boot text is sent.
* `Serial1` or `Serial2`, `CFG_SERIAL_USB=0`: UART is initialized at boot to listen,
  but transmits nothing until bytes arrive. UART cannot detect cable attachment.
  UART pins use the selected core UART defaults.
* The default baud rate is 115200. A terminal holding USB DTR open will immediately
  pause recording again after reboot. Disconnect/close it for unattended recording.

## RAM and MP3 integrity

The PCM ring is **1024 samples / 2048 bytes / 64 ms**, four times the measured
255-sample high-water mark. It has single-producer/single-consumer acquire/release
ordering, and overruns are errors, never silent drops. This is measured headroom,
not a guarantee at every CPU clock, audio setting or workload; check the reported
peak and I2S overflow errors on the actual device.

Before sizing MP3 RAM, firmware mounts storage, starts the I2S DMA buffers and
warms up LAME on core 1. It reserves **24 KiB of additional runtime heap**, then
finds and allocates the largest remaining contiguous block in whole 108-byte MP3
frames. Encoder, DMA, stacks and runtime reserve cannot be used for retained audio.
Capacity is measured at every boot, not guessed from the old free-heap figures.
The maintenance report shows actual allocated frames, bytes and duration:

`retention seconds = allocated frames * 576 / 16000 = allocated bytes / 3000`.

The bit reservoir, ID3 generation and Xing/LAME VBR header are disabled. Thus
overwriting old frames never leaves backward references into discarded audio.
Only complete, validated MPEG-2 mono frames enter the circular buffer; oldest
frames are overwritten, never partial frames. On stop, core 1 drains PCM and
**flushes LAME's delayed tail** before storage reads the ring. New sessions reset
both rings, the compressor and the encoder. Frame duration is 36 ms; the stream
has normal MP3 encoder/decoder delay and padding, without gapless metadata.
Core 1 immediately prepares the next idle encoder before acknowledging stop,
keeping its large heap blocks reserved throughout filesystem operations; this
avoids fragmenting memory needed by the next recording's encoder.
Disabling the reservoir trades some compression quality for independently
decodable circular cuts.

The installed AudioTools `CodecMP3LAME.h` wrapper does not expose CBR bitrate or
reservoir settings and its driver does not flush on close. Consequently
[LameEncoder.cpp](LameEncoder.cpp) uses the already-installed libLAME C API, as
option 7 did, without pulling in the AudioTools umbrella or Maximilian.
It also supplies the fork's `stackhack_*` functions from `src`, replacing the
dependency's allocator when linked from its static archive. The fork caches
masking pointers across `lame_close()`, so freeing its scratch arena leaves the
next encoder session writing into freed memory. A persistent, aligned 30 KiB
arena keeps those pointers valid and out of the recording heap. Its bookkeeping
checks bounds and preserves live allocations when the library frees temporaries
out of order. Like the fork's global quantizer state, this arena supports one
encoder at a time; it must not be reset between sessions. No dependency files
are modified.
The compressor is a small envelope-based soft-knee-free compressor followed by a
hard ceiling; its control names/defaults match the example, but its transfer curve
is explicit here rather than depending on Maximilian's implementation.

## Button, storage and LEDs

* A debounced **short press/release** stops capture, drains/flushes encoding, saves
  the retained audio, then resets and restarts recording. Saving is not continuous
  capture: audio during saving/reinitialization is intentionally not recorded.
* Holding the button **2.5 seconds** requests software halt without setting the
  low-battery flag. A low-battery halt sets the EEPROM flag **before** attempting
  the final save. Both halt paths try to save unsaved audio.
* Every save tries **SD first, then LittleFS** on missing/full/error storage.
  Filenames are `/REC00000001.mp3`, etc.; existing `.mp3` and `.tmp` names are
  skipped, including across boots. Existing recordings are never overwritten.
* Files are written under a temporary name, flushed/closed, read back and
  CRC-checked, then renamed. Any failed target is cleaned up where possible and
  its error is shown in maintenance. Cleanup failure is reported too.
* If both targets fail, recording stops, **the RAM snapshot stays intact**, red
  error pulses start and Serial maintenance is enabled. Option 9 retries saving
  after inserting an SD card or freeing space. Resetting/powering down loses RAM.
* Neither storage is auto-formatted. A blank/unmountable LittleFS requires explicit
  `F` then `FORMAT`, or a correctly formatted SD. Erase-all only removes files;
  it does not repair an unmountable filesystem.
* At boot, no writable capacity on either target opens error maintenance instead
  of silently recording audio that cannot be saved.

All LED timing is cooperative, with **25 ms pulses** rather than blocking delays:

| Color / period | Meaning |
|---|---|
| Blue / 1 second | Boot or paused maintenance |
| Green / 2 seconds | Recording |
| Cyan / 200 ms | Saving, copying or XMODEM |
| Red / 250 ms | Error |
| Yellow / 250 ms for 2 seconds, then dark | Low battery, then halt |
| Dark | Software halted |

The selected halt mechanism is **not electrical power-off**. It ends peripherals,
resets core 1, disables interrupts/SysTick, extinguishes LEDs and waits with WFI
until reset/power cycle. The board/regulator still consumes power. No wake-on-button
is implemented. A persisted low-battery flag prevents restarting below the recovery
threshold; reaching that threshold clears it. A normal cold boot below the low
threshold also halts. Save failures and EEPROM failures are explicitly reported.

## Serial maintenance

Any connection/activity trigger stops/drains recording and retains the snapshot.
Commands are line-oriented (Enter, CR, LF or CRLF). Invalid main options redisplay
all settings, storage capacity/status, PCM peak, MP3 capacity/retention, pins and
the menu. Audio edits reject invalid/trailing/non-finite input and are CRC-protected
binary EEPROM settings with magic, schema version and size. Invalid EEPROM data
restores and persists defaults; EEPROM commit failure disables normal recording.

1. List files; choose `1` LittleFS or `2` SD.
2. Send file by XMODEM/CRC; choose storage, then absolute path.
3. Erase **all files recursively** on chosen storage; type `ERASE` to confirm.
4. Copy **all LittleFS files recursively** to a new SD folder. Enter a folder
   name (letters/digits/underscore/hyphen) or submit an empty line for a randomized
   name. Existing folders are rejected; original LittleFS files are retained.
   Copies are read back and CRC-checked. On failure, a partial destination folder
    can remain and is explicitly reported; choose a fresh name on retry. Long
    operations such as copy/list/erase cancel on a power request; snapshot saves
    finish their current save before halting.
5. Edit audio quality with `number value`, e.g. `2 4.0`. Each valid change commits
   EEPROM; `0` returns. Sampling rate and bitrate remain compile-time constants.
6. Type `SAMPLE` to replace the retained snapshot with exactly 80,000 input samples
   (default five seconds), save using the normal fallback and send the MP3.
7. Send a LittleFS file directly by absolute path.
8. Wait the configured **5000 ms**, then use `rp2040.reboot()`. A warning is printed
   if RAM is unsaved; use option 9 before selecting this option.
9. Save/retry the retained snapshot.
F. Explicitly initialize/format LittleFS; type `FORMAT` to confirm.

Use an XMODEM/CRC receiver on the same connection. The exact byte count is printed
before transfer; XMODEM does not send filenames or exact sizes, so trim any final
128-byte-block SUB padding to the reported size. No status text is injected during
the transfer. LED, button and battery service continue while waiting for serial
data. Depth/path/input limits fail explicitly rather than truncating names.
There is no recording in maintenance; exit/reboot starts a fresh session.

## Build and verification

Use the existing project environments; no dependency/configuration edits outside
`src` are needed:

```sh
~/.platformio/penv/bin/platformio run -e Rp2350ZeroDebug -e zeroTiny
```

Host tests are compiled only with `PASTRECORDER_HOST_TEST`; PlatformIO ignores their
body for firmware builds. The separate storage suite uses
`PASTRECORDER_STORAGE_TEST`. On a Linux host with libmp3lame.so.0:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  -DPASTRECORDER_HOST_TEST -Isrc \
  -I.pio/libdeps/Rp2350ZeroDebug/libLAME/src \
  src/tests/RecorderTests.cpp src/LameEncoder.cpp \
  -l:libmp3lame.so.0 -o /tmp/pastrecorder-tests
/tmp/pastrecorder-tests /tmp/pastrecorder-wrapped.mp3
ffmpeg -v error -i /tmp/pastrecorder-wrapped.mp3 -f null -

g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -DPASTRECORDER_STORAGE_TEST -Isrc/tests/fakes -Isrc \
  src/tests/StorageTests.cpp src/RecordingStore.cpp \
  -o /tmp/pastrecorder-storage-tests
/tmp/pastrecorder-storage-tests /tmp/pastrecorder-storage-fixture
```

Tests check frame-aligned overwrite order, split writes, reservoir/padding
rejection, PCM full/empty/reset and million-sample concurrent ordering, settings
CRC/NaN/range rejection, compressor ceiling, real CBR encoding/flush, repeated
warm-up/encoder sessions, scratch allocation alignment/lifetime/reuse and
out-of-order frees, and a wrapped stream that can be decoded independently.
The storage suite runs the actual save/copy/erase implementation against
file-backed adapters with injected full-media, mount, create, short-write,
corrupt-readback, rename and cleanup failures. It also checks filename collisions,
media reinsertion, recursive copy, source preservation and explicit formatting.
Its fixture directory must not already exist and is removed after success.
The host library does not establish embedded CPU timing or heap headroom.
The same encoder/ring tests were also run against a native build of the exact
installed embedded libLAME sources, and the wrapped output decoded successfully.

On hardware, verify microphone/channel polarity, SD absent/full/write-error
fallback, LittleFS full/unformatted, XMODEM on USB and UART, contact bounce/long
press, EEPROM recovery and battery/recharge hysteresis. Run beyond the reported
capacity and listen to saved wrapped audio. Read the actual PCM peak before
reducing its ring. No hardware upload or battery calibration is performed by
these builds/tests.

## References and battery attribution

* [Working recording example](../src_testBed/main.cpp), option 7.
* [TuriSc/RP2040-Battery-Check](https://github.com/TuriSc/RP2040-Battery-Check):
  [Hardware.cpp](Hardware.cpp) adapts its ADC3/VSYS calibration to cooperative
  Arduino polling, configurable pins and averaging; no interrupt timer or unsafe
  C function-pointer conversions are needed.
* [Arduino-Pico restart example](https://github.com/earlephilhower/arduino-pico/blob/master/libraries/rp2040/examples/RestartReason/RestartReason.ino).
* The supplied ESP32 FunctionalInterrupt example is ESP32-specific. Button
  debouncing is polled here, keeping storage and encoder operations out of ISRs.

Battery reference license:

MIT License

Copyright (c) 2023 Turi Scandurra - https://turiscandurra.com/circuits

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
