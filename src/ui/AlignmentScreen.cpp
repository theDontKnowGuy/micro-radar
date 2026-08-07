#include "ui/AlignmentScreen.h"

#include <Arduino.h>
#include <cmath>

#include "ui/PanelTrim.h"

namespace AlignmentScreen {
namespace {

// The point PanelTrim turns everything about, which is also the point the radar
// face's range rings are drawn around. A pattern for judging a rotation has to
// be symmetric about the point the rotation actually happens around.
constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;

// The outermost ring of pixels the raster has. Whatever the bezel covers, it
// covers from here inwards, so a ring drawn on it shows exactly how much.
constexpr int EDGE_RADIUS = SCREEN_SIZE_DIV_2 - 1;

// What the status screens assume the bezel hides -- see BezelMargin in
// StatusScreen.cpp. Drawn so the guess can be checked against the real one
// rather than taken on trust.
constexpr int BEZEL_MARGIN = 10;

// Three pixels rather than one. The crosshair is the thing being judged for
// level, and with a trim set it arrives through a rotation that resamples it;
// a single-pixel line comes out of that as a broken staircase, which is hard to
// sight along. A thicker one keeps an edge to read.
constexpr int CROSSHAIR_THICKNESS = 3;

constexpr uint32_t CROSSHAIR_COLOR = lgfx::color888(255, 255, 255);
constexpr uint32_t EDGE_COLOR = lgfx::color888(0, 255, 0);
constexpr uint32_t BEZEL_COLOR = lgfx::color888(0, 70, 0);
constexpr uint32_t TICK_COLOR = lgfx::color888(0, 140, 0);
constexpr uint32_t LABEL_COLOR = lgfx::color888(0, 255, 0);

// Compass reckoning -- zero at the top, growing clockwise -- because that is
// how someone holding the thing up against a shelf edge will describe what they
// see. Note this is not the sweep's zero, which is at three o'clock; the two
// answer different questions and there is no sense in making this one harder to
// read for the sake of matching.
void TickEnd(float degrees, int radius, int& x, int& y)
{
  const float radians = degrees * DEG_TO_RAD;
  x = CENTRE + static_cast<int>(std::sin(radians) * radius);
  y = CENTRE - static_cast<int>(std::cos(radians) * radius);
}

}

void Draw(LGFX& tft)
{
  LovyanGFX& canvas = PanelTrim::Canvas(tft);

  canvas.fillScreen(lgfx::color888(0, 0, 0));

  // Every five degrees, with a longer mark on each fifteenth. Five is about the
  // finest spacing that stays separate at this radius, and it is also roughly
  // the smallest tilt worth correcting, so a rim that reads as one tick out is
  // a rim worth trimming.
  for (int degrees = 0; degrees < 360; degrees += 5) {
    const bool major = degrees % 15 == 0;
    int outerX, outerY, innerX, innerY;
    TickEnd(degrees, EDGE_RADIUS, outerX, outerY);
    TickEnd(degrees, EDGE_RADIUS - (major ? 16 : 8), innerX, innerY);
    canvas.drawLine(outerX, outerY, innerX, innerY, TICK_COLOR);
  }

  canvas.drawCircle(CENTRE, CENTRE, EDGE_RADIUS, EDGE_COLOR);
  canvas.drawCircle(CENTRE, CENTRE, EDGE_RADIUS - BEZEL_MARGIN, BEZEL_COLOR);

  // Drawn over the ticks rather than under them: where the two meet is the
  // reading being taken, and the crosshair is what the reading is of.
  canvas.fillRect(0, CENTRE - CROSSHAIR_THICKNESS / 2,
                  SCREEN_SIZE, CROSSHAIR_THICKNESS, CROSSHAIR_COLOR);
  canvas.fillRect(CENTRE - CROSSHAIR_THICKNESS / 2, 0,
                  CROSSHAIR_THICKNESS, SCREEN_SIZE, CROSSHAIR_COLOR);

  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(LABEL_COLOR, lgfx::color888(0, 0, 0));

  // Only the quarters are labelled. Every fifteenth would be readable at this
  // size but would also crowd the rim, and the crosshair already says where the
  // quarters are -- these are here to name them, not to find them.
  canvas.setTextSize(2);
  const int labelRadius = EDGE_RADIUS - 34;
  for (int degrees = 0; degrees < 360; degrees += 90) {
    int x, y;
    TickEnd(degrees, labelRadius, x, y);
    canvas.drawCentreString(String(degrees), x, y - 7);
  }

  // What the radar is currently correcting by, so the pattern on screen is
  // never ambiguous about which state it is showing.
  canvas.setTextSize(3);
  canvas.drawCentreString("TRIM " + String(PanelTrim::Degrees(), 1),
                          CENTRE, CENTRE + 40);

  canvas.setTextSize(1);
  PanelTrim::Present(tft);

  Serial.printf("Alignment pattern shown; trim is %.1f degrees. "
                "Clear the alignment checkbox on the configuration page when done.\n",
                PanelTrim::Degrees());
}

}
