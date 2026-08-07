#include "ui/UpdateScreen.h"

#include <Arduino.h>

#include "FirmwareVersion.h"
#include "ui/PanelTrim.h"
#include "ui/ProgressBar.h"

namespace UpdateScreen {

void RunFirmwareUpdate(LGFX& tft, FirmwareUpdater& updater)
{
  const FirmwareUpdater::Release release = updater.PendingRelease();
  Serial.printf("Updating firmware: %s -> %s\n", FIRMWARE_VERSION, release.version.c_str());

  LovyanGFX& canvas = PanelTrim::Canvas(tft);

  tft.waitDMA();
  canvas.fillScreen(lgfx::color888(0, 0, 0));
  // Opaque text: the percentage below is redrawn in place, and without a
  // background colour each repaint would smear over the last one.
  canvas.setTextColor(lgfx::color888(0, 255, 0), lgfx::color888(0, 0, 0));

  const int lineHeight = canvas.fontHeight() + 10;
  canvas.drawCentreString("Updating firmware", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight * 2);
  canvas.drawCentreString(release.version, SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight);

  const int barY = SCREEN_SIZE_DIV_2 + lineHeight;
  ProgressBar::DrawOutline(canvas, barY, lgfx::color888(0, 255, 0));
  PanelTrim::Present(tft);

  // Redraw only when the figure has moved by a step. The download is ~1.3MB in
  // small chunks, and repainting the bar on every one of them would put more
  // SPI traffic in the way of the transfer than the progress is worth.
  //
  // The step is one percent normally and five on a trimmed unit, because there
  // the repaint is not two small fills but a whole turned frame -- every
  // scanline of one is read back and written again individually, which on this
  // panel is around 150ms. A hundred of those would add a quarter-minute to the
  // update and spend it competing with the download for the bus. Twenty still
  // reads as a bar that moves.
  const int percentStep = PanelTrim::Degrees() == 0.0f ? 1 : 5;
  int lastPercent = -1;
  const bool installed = updater.Install(release,
    [&](size_t written, size_t total) {
      const int percent = static_cast<int>((written * 100) / total);
      if (percent < lastPercent + percentStep && percent < 100)
        return;
      if (percent == lastPercent)
        return;
      lastPercent = percent;

      ProgressBar::DrawFill(canvas, barY, percent, lgfx::color888(0, 255, 0));
      canvas.drawCentreString(String(percent) + "%   ", SCREEN_SIZE_DIV_2, barY + ProgressBar::Height + 10);
      PanelTrim::Present(tft);
    });

  if (installed) {
    canvas.fillScreen(lgfx::color888(0, 0, 0));
    canvas.drawCentreString("Restarting", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2);
    PanelTrim::Present(tft);
    delay(1000);
    tft.waitDMA();
    ESP.restart();
  }

  // The running firmware is untouched -- a failed write only dirties the
  // inactive slot -- but the network worker was suspended by the caller and
  // cannot be resumed safely, so the radar would sit on stale aircraft. Reboot
  // instead: it comes back on the current version and retries an hour later.
  canvas.fillScreen(lgfx::color888(0, 0, 0));
  canvas.drawCentreString("Update failed", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight);
  canvas.drawCentreString("Restarting", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2);
  PanelTrim::Present(tft);
  delay(3000);
  tft.waitDMA();
  ESP.restart();
}

}
