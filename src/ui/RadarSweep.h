#pragma once

#include "LGFX.h"

class ConfigurationWebServer;

// The rotating sweep the radar face is named after.
namespace RadarSweep {

struct Settings {
    bool enabled = true;
    unsigned long periodMs = 5000;
};

// Reads the display configuration once, at startup. Reading Preferences in
// every frame causes visible NVS-related frame-time spikes.
[[nodiscard]] Settings LoadSettings(ConfigurationWebServer& configServer);

// Draws the sweep into the backbuffer for the frame at `now` and returns the
// angle its leading edge is at, which is what aircraft are revealed and
// advanced by. Returns 0 without drawing when the sweep is switched off.
float Draw(LGFX_Sprite& backbuffer, const Settings& settings, unsigned long now);

}
