#pragma once

#include <stdint.h>
/**
## Settings for distant or quiet sounds

| Goal | Setting |
|---|---|
| Compress more of the signal, not just peaks | Low threshold, about `0.05`–`0.1` (−26 to −20 dBFS) |
| Pull loud nearby sounds down hard | Ratio `4`–`10` |
| Catch sudden loud sounds quickly | Attack `5`–`20` ms |
| Let quiet sounds come back up quickly | Release `100`–`250` ms |
| Make everything louder | Makeup gain ×4 to ×10 (+12 to +20 dB) |
| Prevent clipping | Final hard limiter or clamp at about `0.95` |
| Remove low-frequency rumble, wind, and handling noise | High-pass filter at 100–200 Hz, placed before the compressor |
| Avoid amplifying hiss during silence | Noise gate or expander (optional) |

 */
// Maximilian pulls `using namespace std;` into every includer, so it stays in VoiceCompressor.cpp.

// Resets the compressor state for a mono stream at sampleRate.
void voiceCompressorBegin(uint32_t sampleRate);

// Compresses one full-scale signed 32-bit sample.
int32_t voiceCompressorProcess(int32_t sample);
