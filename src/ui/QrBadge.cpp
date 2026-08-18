#include "ui/QrBadge.h"

#include <Arduino.h>

namespace QrBadge {
namespace {

// White modules on a black plate, where the rest of the panel UI is green on
// black. The odd one out on purpose: this is the only thing on any of these
// screens that is read by a camera rather than by a person.
//
// Green costs a code twice over. It is the dimmer half of the contrast a
// decoder is looking for, and -- the part that actually matters here -- on an
// RGB stripe panel a pure green fill lights one subpixel in three. A module
// four pixels wide is then four narrow green stripes with dark gaps between
// them, which is a finer and fainter pattern than the module grid it is meant
// to be, and it is the module grid a decoder has to recover. White lights all
// three, so a module is a solid block of the width it claims.
constexpr uint32_t PLATE_COLOR = lgfx::color888(0, 0, 0);
constexpr uint32_t MODULE_COLOR = lgfx::color888(255, 255, 255);

int PlateModules(const Code& code)
{
  return code.size + 2 * code.quietZone;
}

bool IsDark(const Code& code, int x, int y)
{
  const uint8_t packed = pgm_read_byte(&code.modules[y * code.stride + x / 8]);
  return (packed >> (7 - (x % 8))) & 1;
}

}

int SizeWithin(const Code& code, int maxSize)
{
  const int plateModules = PlateModules(code);
  return (maxSize / plateModules) * plateModules;
}

void Draw(LovyanGFX& canvas, const Code& code, int x, int y, int size)
{
  const int scale = size / PlateModules(code);
  if (scale < 1)
    return;

  const int plate = scale * PlateModules(code);

  // Where the code itself starts, inside the quiet zone.
  const int codeX = x + code.quietZone * scale;
  const int codeY = y + code.quietZone * scale;

  // Softened corners, because the plate is a square on a round panel and a hard
  // corner is the thing that makes that look like a mistake. Two modules, so
  // the curve stays well inside the quiet zone and cannot reach the finder
  // patterns.
  canvas.fillRoundRect(x, y, plate, plate, 2 * scale, PLATE_COLOR);

  // A rectangle per run of dark modules rather than per module. With a trim set
  // this is drawing into memory and it makes no odds, but without one every
  // rectangle is a window command to the panel over SPI, and the rows here are
  // mostly runs -- the finder patterns alone are seven modules wide.
  canvas.startWrite();
  for (int row = 0; row < code.size; row++) {
    int runStart = -1;
    for (int column = 0; column <= code.size; column++) {
      const bool dark = column < code.size && IsDark(code, column, row);
      if (dark && runStart < 0) {
        runStart = column;
      } else if (!dark && runStart >= 0) {
        canvas.fillRect(codeX + runStart * scale,
                        codeY + row * scale,
                        (column - runStart) * scale,
                        scale,
                        MODULE_COLOR);
        runStart = -1;
      }
    }
  }
  canvas.endWrite();
}

}
