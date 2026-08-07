#include "ui/RadarSweep.h"

#include <Arduino.h>
#include <cmath>

#include "ConfigurationWebServer.h"

namespace RadarSweep {
namespace {

constexpr int SWEEP_THICKNESS = 20;
constexpr int SWEEP_SPACING = 5;

// The sweep itself: a fan of lines from the centre, each one a little dimmer
// and a little further round than the last, so the trail fades out behind the
// bright leading edge drawn on the end.
void DrawScanLines(LGFX_Sprite& buf, const int x0, const int y0, const int x1, const int y1, const int thickness, const int trailBrightness, const int spacing)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrt(dx * dx + dy * dy);

    // perpendicular unit vector
    float px = -dy / len;
    float py = dx / len;

    for (int i = 0; i <= thickness; i++) {
        // 1.0 at centre, 0.0 at edges
        float t = i / (float)(thickness);
        uint8_t brightness = (uint8_t)(t * trailBrightness);

        buf.drawLine(
            x0, y0,
            x1 + (px * (i * spacing)), y1 + (py * (i * spacing)),
            lgfx::color888(0, brightness, 0)
        );
    }

    buf.drawLine(
        x0, y0,
        x1 + (px * (thickness * spacing)), y1 + (py * (thickness * spacing)),
        lgfx::color888(0, 200, 0)
    );
}

}

Settings LoadSettings(ConfigurationWebServer& configServer)
{
    Settings settings;

    const String scanlineSetting = configServer.GetStoredString("scanline");
    settings.enabled = scanlineSetting.isEmpty() || scanlineSetting == "true";

    long sweepPeriodSeconds = configServer.GetStoredString("sweep-period").toInt();
    if (sweepPeriodSeconds < 2 || sweepPeriodSeconds > 60)
        sweepPeriodSeconds = 5;
    settings.periodMs = static_cast<unsigned long>(sweepPeriodSeconds) * 1000UL;

    return settings;
}

float Draw(LGFX_Sprite& backbuffer, const Settings& settings, unsigned long now)
{
    if (!settings.enabled)
        return 0.0f;

    const float sweepAngle = (now % settings.periodMs) * (TWO_PI / settings.periodMs);
    DrawScanLines(backbuffer,
        SCREEN_SIZE_DIV_2 - 1,
        SCREEN_SIZE_DIV_2 - 1,
        SCREEN_SIZE_DIV_2 - 1 + (std::cos(sweepAngle) * SCREEN_SIZE_DIV_2),
        SCREEN_SIZE_DIV_2 - 1 + (std::sin(sweepAngle) * SCREEN_SIZE_DIV_2),
        SWEEP_THICKNESS, 128, SWEEP_SPACING
    );

    // DrawScanLines makes its brightest leading edge by offsetting the final
    // ray perpendicular to the base angle. Use that same edge to reveal and
    // advance aircraft, so the visual crossing and data update coincide.
    static const float SWEEP_LEADING_EDGE_OFFSET = std::atan2(
        static_cast<float>(SWEEP_THICKNESS * SWEEP_SPACING),
        static_cast<float>(SCREEN_SIZE_DIV_2)
    );

    float sweepUpdateAngle = sweepAngle + SWEEP_LEADING_EDGE_OFFSET;
    if (sweepUpdateAngle >= TWO_PI)
        sweepUpdateAngle -= TWO_PI;

    return sweepUpdateAngle;
}

}
