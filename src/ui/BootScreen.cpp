#include "ui/BootScreen.h"

#include <Arduino.h>

#include "BootLogo.h"
#include "ui/ProgressBar.h"

namespace BootScreen {
namespace {

// Picked off the logo artwork so the bar reads as part of it rather than as
// something drawn on top: the bright blue of the wordmark, and a dim version
// of the same hue for the trough.
constexpr uint32_t BAR_COLOR = lgfx::color888(41, 163, 232);
constexpr uint32_t BAR_TROUGH_COLOR = lgfx::color888(16, 62, 112);

// Below the wordmark, inside the ring, where the artwork is black.
constexpr int BAR_Y = SCREEN_SIZE_DIV_2 + 70;

}

void Draw(LGFX& tft)
{
  tft.fillScreen(lgfx::color888(0, 0, 0));

  lgfx::rgb565_t scanline[BootLogo::Width];
  size_t runIndex = 0;
  uint16_t runColor = 0;
  uint16_t runRemaining = 0;
  const int logoX = (SCREEN_SIZE - BootLogo::Width) / 2;
  const int logoY = (SCREEN_SIZE - BootLogo::Height) / 2;

  tft.startWrite();
  for (uint16_t y = 0; y < BootLogo::Height; y++) {
    for (uint16_t x = 0; x < BootLogo::Width; x++) {
      if (runRemaining == 0 && runIndex < BootLogo::RunCount) {
        const BootLogo::Run& run = BootLogo::Runs[runIndex++];
        runColor = pgm_read_word(&run.color);
        runRemaining = pgm_read_word(&run.length);
      }

      scanline[x] = lgfx::rgb565_t(runColor);
      if (runRemaining > 0)
        runRemaining--;
    }

    tft.pushImage(logoX, logoY + y, BootLogo::Width, 1, scanline);
  }
  tft.endWrite();

  ProgressBar::DrawOutline(tft, BAR_Y, BAR_TROUGH_COLOR);
}

void Hold(LGFX& tft, unsigned long startedAt)
{
  // Repainted only when the whole-percent figure moves, same as the update
  // screen: 100 small fills over seven seconds, not one per pass of the loop.
  int lastPercent = -1;
  unsigned long elapsed = 0;
  while ((elapsed = millis() - startedAt) < HoldMs) {
    const int percent = static_cast<int>((elapsed * 100) / HoldMs);
    if (percent != lastPercent) {
      lastPercent = percent;
      ProgressBar::DrawFill(tft, BAR_Y, percent, BAR_COLOR);
    }
    delay(10);
  }

  // The loop above always stops a percent or two short of the end.
  ProgressBar::DrawFill(tft, BAR_Y, 100, BAR_COLOR);

  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.waitDMA();
}

}
