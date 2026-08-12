#include "ui/ConfigQr.h"

#include <Arduino.h>

#include "ConfigurationWebServer.h"
#include "QrCode.h"

namespace ConfigQr {
namespace {

// Match the rest of the panel UI: green modules on a black plate.
constexpr uint32_t PLATE_COLOR = lgfx::color888(0, 0, 0);
constexpr uint32_t MODULE_COLOR = lgfx::color888(0, 255, 0);

// The code plus the clear space it needs on all four sides. Both come from the
// generated header, so a longer address -- which would mean a bigger version
// and more modules -- makes every badge smaller rather than making one of them
// overrun.
constexpr int PLATE_MODULES = QrCode::Size + 2 * QrCode::QuietZone;

// What the badge claims the radar answers to, checked against what the radar
// actually answers to. The picture is generated from MdnsAddress by a script,
// and a script that has not been run since the name changed leaves a code that
// looks perfectly good and goes nowhere -- the one kind of mistake that survives
// being looked at. So the build fails instead.
constexpr bool SameText(const char* left, const char* right)
{
  return *left == *right && (*left == '\0' || SameText(left + 1, right + 1));
}

constexpr bool StartsWith(const char* text, const char* prefix)
{
  return *prefix == '\0' || (*text == *prefix && StartsWith(text + 1, prefix + 1));
}

// The scheme is part of what is encoded on purpose: a phone offers to open a
// string that starts with one and offers to search for a string that does not.
constexpr int SchemeLength = 7;  // "http://"

static_assert(StartsWith(QrCode::Url, "http://")
              && SameText(QrCode::Url + SchemeLength, MdnsAddress),
              "the QR code and MdnsAddress disagree - "
              "rerun scripts/generate_qr_code.py");

bool IsDark(int x, int y)
{
  const uint8_t packed = pgm_read_byte(&QrCode::Modules[y * QrCode::Stride + x / 8]);
  return (packed >> (7 - (x % 8))) & 1;
}

}

int SizeWithin(int maxSize)
{
  return (maxSize / PLATE_MODULES) * PLATE_MODULES;
}

void Draw(LovyanGFX& canvas, int x, int y, int size)
{
  const int scale = size / PLATE_MODULES;
  if (scale < 1)
    return;

  const int plate = scale * PLATE_MODULES;

  // Where the code itself starts, inside the quiet zone.
  const int codeX = x + QrCode::QuietZone * scale;
  const int codeY = y + QrCode::QuietZone * scale;

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
  for (int row = 0; row < QrCode::Size; row++) {
    int runStart = -1;
    for (int column = 0; column <= QrCode::Size; column++) {
      const bool dark = column < QrCode::Size && IsDark(column, row);
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
