#include "ui/UpdateScreen.h"

#include <Arduino.h>

#include "FirmwareVersion.h"
#include "ui/ProgressBar.h"

namespace UpdateScreen {

void RunFirmwareUpdate(LGFX& tft, FirmwareUpdater& updater)
{
  const FirmwareUpdater::Release release = updater.PendingRelease();
  Serial.printf("Updating firmware: %s -> %s\n", FIRMWARE_VERSION, release.version.c_str());

  tft.waitDMA();
  tft.fillScreen(lgfx::color888(0, 0, 0));
  // Opaque text: the percentage below is redrawn in place, and without a
  // background colour each repaint would smear over the last one.
  tft.setTextColor(lgfx::color888(0, 255, 0), lgfx::color888(0, 0, 0));

  const int lineHeight = tft.fontHeight() + 10;
  tft.drawCentreString("Updating firmware", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight * 2);
  tft.drawCentreString(release.version, SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight);

  const int barY = SCREEN_SIZE_DIV_2 + lineHeight;
  ProgressBar::DrawOutline(tft, barY, lgfx::color888(0, 255, 0));

  // Redraw only when the whole-percent figure moves. The download is ~1.3MB in
  // small chunks, and repainting the bar on every one of them would put more
  // SPI traffic in the way of the transfer than the progress is worth.
  int lastPercent = -1;
  const bool installed = updater.Install(release,
    [&](size_t written, size_t total) {
      const int percent = static_cast<int>((written * 100) / total);
      if (percent == lastPercent)
        return;
      lastPercent = percent;

      ProgressBar::DrawFill(tft, barY, percent, lgfx::color888(0, 255, 0));
      tft.drawCentreString(String(percent) + "%   ", SCREEN_SIZE_DIV_2, barY + ProgressBar::Height + 10);
    });

  if (installed) {
    tft.fillScreen(lgfx::color888(0, 0, 0));
    tft.drawCentreString("Restarting", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2);
    delay(1000);
    tft.waitDMA();
    ESP.restart();
  }

  // The running firmware is untouched -- a failed write only dirties the
  // inactive slot -- but the network worker was suspended by the caller and
  // cannot be resumed safely, so the radar would sit on stale aircraft. Reboot
  // instead: it comes back on the current version and retries an hour later.
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.drawCentreString("Update failed", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight);
  tft.drawCentreString("Restarting", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2);
  delay(3000);
  tft.waitDMA();
  ESP.restart();
}

}
