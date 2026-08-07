#pragma once

#include <Arduino.h>

namespace FrameTimer {

// Set true to log per-frame draw/push timing, frame rate and free heap over
// serial every 2s. Handy when tuning the SPI clock or tracking down a
// frame-rate or memory regression; off by default so the log stays readable.
//
// Worth knowing what the numbers should look like before reading new ones: a
// 360x360 frame at 16 bits is 259,200 bytes, and 80MHz is the fastest this chip
// will clock the bus, so roughly 27ms of the push is pure transfer and no
// change here can touch it. Around 12ms of draw against that is the radar
// behaving; 25fps is the ceiling, not a regression.
constexpr bool Enabled = false;

void Accumulate(uint32_t drawUs, uint32_t pushUs, unsigned long now, unsigned long sweepPeriodMs);

// `if constexpr` at the call site so that with logging off neither the counters
// nor their per-frame arithmetic are merely unread but absent -- this is the
// hot path, and --gc-sections drops the rest of the module with the call.
inline void Record(uint32_t drawUs, uint32_t pushUs, unsigned long now, unsigned long sweepPeriodMs)
{
    if constexpr (Enabled)
        Accumulate(drawUs, pushUs, now, sweepPeriodMs);
}

}
