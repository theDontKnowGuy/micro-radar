#include "ui/QrScreen.h"

#include <Arduino.h>

#include "QrCode.h"
#include "ui/PanelTrim.h"

namespace QrScreen {
namespace {

// A QR is read off the contrast between its modules, so it goes on white and
// stays black -- not the panel's green on black. A phone will read an inverted
// code, but only the ones that think to try.
constexpr uint32_t PLATE_COLOR = lgfx::color888(255, 255, 255);
constexpr uint32_t MODULE_COLOR = lgfx::color888(0, 0, 0);

// The code plus the clear space it needs on all four sides. Both come from the
// generated header, so a longer URL -- which would mean a bigger version and
// more modules -- resizes everything below rather than overrunning the panel.
constexpr int PLATE_MODULES = QrCode::Size + 2 * QrCode::QuietZone;

// The largest square that fits inside a round panel is its inscribed one, and
// this is the side of it. What actually gets drawn is the whole-module scale
// below, which is smaller again.
constexpr int PLATE_LIMIT = static_cast<int>(SCREEN_SIZE / 1.4142136f);

// Whole pixels per module, always. A module drawn 6.2 pixels wide is a module
// drawn 6 pixels wide most of the time and 7 the rest, and a decoder reading a
// grid whose spacing wanders is a decoder that gives up -- which is worth more
// than the four pixels a side that rounding down costs here.
constexpr int SCALE = PLATE_LIMIT / PLATE_MODULES;
constexpr int PLATE_SIZE = SCALE * PLATE_MODULES;
constexpr int PLATE_X = (SCREEN_SIZE - PLATE_SIZE) / 2;

// Where the code itself starts, inside the quiet zone.
constexpr int CODE_X = PLATE_X + QrCode::QuietZone * SCALE;

// Softened corners, because the plate is a square on a round panel and a hard
// corner is the thing that makes that look like a mistake. Two modules, so the
// curve stays well inside the quiet zone and cannot reach the finder patterns.
constexpr int PLATE_RADIUS = 2 * SCALE;

static_assert(SCALE >= 2, "the code has outgrown the panel");

bool IsDark(int x, int y)
{
  const uint8_t packed = pgm_read_byte(&QrCode::Modules[y * QrCode::Stride + x / 8]);
  return (packed >> (7 - (x % 8))) & 1;
}

}

void Show(LGFX& tft)
{
  Serial.printf("Showing the QR code for %s\n", QrCode::Url);

  LovyanGFX& canvas = PanelTrim::Canvas(tft);
  canvas.fillScreen(lgfx::color888(0, 0, 0));
  canvas.fillRoundRect(PLATE_X, PLATE_X, PLATE_SIZE, PLATE_SIZE, PLATE_RADIUS, PLATE_COLOR);

  // A rectangle per run of dark modules rather than per module. With a trim set
  // this is drawing into memory and it makes no odds, but without one every
  // rectangle is a window command to the panel over SPI, and the rows here are
  // mostly runs -- the finder patterns alone are seven modules wide.
  canvas.startWrite();
  for (int y = 0; y < QrCode::Size; y++) {
    int runStart = -1;
    for (int x = 0; x <= QrCode::Size; x++) {
      const bool dark = x < QrCode::Size && IsDark(x, y);
      if (dark && runStart < 0) {
        runStart = x;
      } else if (!dark && runStart >= 0) {
        canvas.fillRect(CODE_X + runStart * SCALE,
                        CODE_X + y * SCALE,
                        (x - runStart) * SCALE,
                        SCALE,
                        MODULE_COLOR);
        runStart = -1;
      }
    }
  }
  canvas.endWrite();

  PanelTrim::Present(tft);

  delay(HoldMs);

  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.waitDMA();
}

}
