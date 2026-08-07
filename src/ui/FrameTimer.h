#pragma once

#include <Arduino.h>

namespace FrameTimer {

// Set true to log per-frame draw/push timing, frame rate and free heap over
// serial every 2s. Handy when tuning the SPI clock or tracking down a
// frame-rate or memory regression; off by default so the log stays readable.
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
