#pragma once

#include "LGFX.h"

class ConfigurationWebServer;

// The rotating sweep the radar face is named after.
namespace RadarSweep {

struct Settings {
    bool enabled = true;
    unsigned long periodMs = 5000;
};

// The sweep's position for one frame, worked out from `now` alone. Split out
// from drawing so a caller can react to where the beam is -- reveal an
// aircraft, latch a clock figure -- before it is actually painted, which
// matters once something else is meant to sit visually behind the beam: that
// something has to be drawn first, using this frame's position, and the beam
// drawn over it afterwards.
struct SweepState {
    float angle = 0.0f;        // Raw angle, 0 at three o'clock, increasing clockwise.
    float updateAngle = 0.0f;  // Leading edge of the fan -- see DrawFan.
};

// Reads the display configuration once, at startup. Reading Preferences in
// every frame causes visible NVS-related frame-time spikes.
[[nodiscard]] Settings LoadSettings(ConfigurationWebServer& configServer);

// Works out the sweep's position for the frame at `now`, without drawing
// anything. Zero-valued when the sweep is switched off.
[[nodiscard]] SweepState Compute(const Settings& settings, unsigned long now);

// Draws the sweep fan into the backbuffer at the position `state` holds.
// Whatever is already on the backbuffer where the fan lands is painted over,
// which is the point: this is called last among the layers that want the
// beam sweeping over them. A no-op when the sweep is switched off.
void DrawFan(LGFX_Sprite& backbuffer, const Settings& settings, const SweepState& state);

}
