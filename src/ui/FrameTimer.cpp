#include "ui/FrameTimer.h"

namespace FrameTimer {

// The sprite push is SCREEN_SIZE^2 * 2 bytes over SPI and dominates the frame,
// so the SPI clock sets the frame rate directly. Reported as degrees-of-sweep
// per frame because that is what actually reads as smooth or jumpy.
void Accumulate(uint32_t drawUs, uint32_t pushUs, unsigned long now, unsigned long sweepPeriodMs)
{
    static uint32_t frameCount = 0;
    static uint32_t drawUsTotal = 0;
    static uint32_t pushUsTotal = 0;
    static unsigned long lastTimingReportAt = 0;

    drawUsTotal += drawUs;
    pushUsTotal += pushUs;
    frameCount++;

    if (now - lastTimingReportAt < 2000)
        return;

    lastTimingReportAt = now;
    const float drawMs = (drawUsTotal / 1000.0f) / frameCount;
    const float pushMs = (pushUsTotal / 1000.0f) / frameCount;
    const float totalMs = drawMs + pushMs;
    Serial.printf(
        "frame: draw %.1fms  push %.1fms  total %.1fms  %.1f fps  %.1f deg/frame"
        "  heap %u (largest %u)\n",
        drawMs, pushMs, totalMs,
        totalMs > 0.0f ? 1000.0f / totalMs : 0.0f,
        360.0f * (totalMs / static_cast<float>(sweepPeriodMs)),
        ESP.getFreeHeap(), ESP.getMaxAllocHeap()
    );
    frameCount = 0;
    drawUsTotal = 0;
    pushUsTotal = 0;
}

}
