#include <Arduino.h>
#include <WiFi.h>

#include "AircraftManager.h"
#include "ConfigurationWebServer.h"
#include "FirmwareUpdater.h"
#include "FirmwareVersion.h"
#include "HttpRequestManager.h"
#include "LGFX.h"
#include "OpenSkyAuthTokenHandler.h"
#include "WiFiConnection.h"
#include "ui/BootScreen.h"
#include "ui/FrameTimer.h"
#include "ui/RadarSweep.h"
#include "ui/StatusScreen.h"
#include "ui/UpdateScreen.h"

LGFX tft;
LGFX_Sprite backbuffer(&tft);

ConfigurationWebServer configServer;
HttpRequestManager http;
OpenSkyAuthTokenHandler authHandler(http);

AircraftManager aircraftManager(configServer, authHandler, http);
FirmwareUpdater firmwareUpdater;

RadarSweep::Settings sweepSettings;

void setup()
{
  Serial.begin(115200);
  // delay(1000); // avoids immediate serial output being cut off - uncomment if needed

  // initialise LGFX + screen
  tft.init();
  tft.invertDisplay(DISPLAY_INVERT); // see DisplayConfig.h
  // The backlight starts off (see LGFX.h). Clear the panel first so it is not
  // lit up on whatever random content its RAM powered on with.
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setBrightness(255);
  BootScreen::Draw(tft);
  const unsigned long bootStartedAt = millis();

  // At 360x360 the backbuffer is 129,600 bytes. Keeping that in SRAM leaves too
  // little contiguous heap for the TLS handshake OpenSky needs -- requests fail
  // with "SSL - Memory allocation failed" -- so put it in PSRAM, which the
  // required N16R8 module has. The check only guards the allocation below from
  // asking for PSRAM on a board that has none.
  if (ESP.getPsramSize() > 0)
    backbuffer.setPsram(true);

  backbuffer.setColorDepth(8);
  if (!backbuffer.createSprite(SCREEN_SIZE, SCREEN_SIZE)) {
    Serial.printf("FATAL: could not allocate %dx%d backbuffer (%d bytes)\n",
                  SCREEN_SIZE, SCREEN_SIZE, SCREEN_SIZE * SCREEN_SIZE);
  }

  WiFi.mode(WIFI_STA);

  // The stored network decides which mode the page is served in, so the
  // settings have to be readable before that decision is made.
  configServer.PrepareStorage();

  // Start the join before holding the logo, not after it, and let the logo
  // cover the whole join budget. The association takes a second or two on a
  // good day and the logo is up for seven regardless, so running them together
  // takes that time off the boot rather than adding to it; a router that needs
  // longer gets it without the screen changing to say so.
  WiFiConnection::BeginJoin(configServer);
  BootScreen::Hold(tft, bootStartedAt, WiFiConnection::JoinPending, WiFiConnection::JoinTimeoutMs);

  // No network, and no way to be configured except by serving the page over an
  // access point of the radar's own; that does not return.
  if (!WiFiConnection::FinishJoin())
    WiFiConnection::RunSetupPortal(tft, configServer, firmwareUpdater);

  // Start the clock on the radar face. Kept in UTC deliberately -- both offset
  // arguments are zero -- because this chip has no timezone database to turn a
  // place into an offset; AircraftManager fetches the offset for the configured
  // coordinates and applies it when it draws. Asynchronous: the first sync
  // lands a few seconds from here, and the clock stays off screen until it has.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // begin background server for configuration
  configServer.Initialise(firmwareUpdater, ConfigurationWebServer::Mode::Station);

  const String mdnsUrl = String("http://") + MdnsAddress;
  const String configurationUrl = String("http://") + WiFi.localIP().toString();
  Serial.print("Configure me at ");
  Serial.print(mdnsUrl);
  Serial.print(" or ");
  Serial.println(configurationUrl);

  // Once the radar starts updating itself the version on screen is the only way
  // to tell, at a glance, which build a given unit ended up on. Kept to the bare
  // number here -- the release date and description are on the configuration
  // page, where there is room for them.
  //
  // Both addresses are shown, name first: the name survives the router handing
  // out a different lease, but it needs an mDNS resolver at the other end, which
  // not every phone or network has. The IP is the fallback that always works.
  //
  // Scheme and address stay on one line: a URL split across two reads as two
  // things to type. The fallback line carries the bare IP rather than a second
  // http:// -- ShowStatusScreen sizes the type to the longest line, and the
  // scheme twice would cost a step of size for something already said above.
  ShowStatusScreen(tft,
                   "Configure me at:",
                   mdnsUrl,
                   "or " + WiFi.localIP().toString(),
                   "v" FIRMWARE_VERSION);

  // Keep the address visible long enough to read while the asynchronous
  // configuration server is already available.
  delay(7000);

  // initialise aircraft manager
  aircraftManager.Initialise();

  // Start polling GitHub for new firmware. Needs Wi-Fi, so it goes after the
  // join; the checker delays its first look by a couple of minutes. Whether a
  // find installs itself or waits for the owner is read here rather than per
  // check -- saving the setting reboots the radar anyway.
  const String autoUpdateSetting = configServer.GetStoredString("auto-update");
  firmwareUpdater.SetAutoInstall(autoUpdateSetting != "false");
  firmwareUpdater.Initialise();

  sweepSettings = RadarSweep::LoadSettings(configServer);
}

void loop()
{
  // Settings were saved over the web UI. Restart here rather than in the
  // request handler, so no SPI DMA transfer is in flight when the chip resets.
  if (configServer.RestartRequested()) {
    Serial.println("Settings saved - restarting");
    delay(200);   // let the HTTP response finish going out
    tft.waitDMA(); // make sure the panel bus is idle
    ESP.restart();
  }

  // Same reasoning as the restart above: the updater's background task found a
  // new release, but the flash and reboot happen here, between frames, with the
  // panel idle.
  if (firmwareUpdater.UpdatePending()) {
    // A firmware download and an OpenSky fetch both want a TLS session, and
    // this board does not have the contiguous heap for two.
    aircraftManager.SuspendNetworkTask();
    UpdateScreen::RunFirmwareUpdate(tft, firmwareUpdater);
    return;
  }

  // "Ask me first" only asks if something says so where the radar is actually
  // being looked at, which for most units is the panel rather than the
  // configuration page. Three atomic loads, so it can just track the flag.
  aircraftManager.ShowUpdateNotice(firmwareUpdater.AwaitingConfirmation());

  aircraftManager.Update();

  // Keep presentation cadence stable even when the rest of the loop completes
  // at slightly different speeds. Network work runs in AircraftManager's
  // background task and no longer blocks this schedule.
  constexpr unsigned long FRAME_INTERVAL_MS = 33; // approximately 30 FPS
  static unsigned long lastFrameAt = 0;
  const unsigned long now = millis();
  if (now - lastFrameAt < FRAME_INTERVAL_MS) {
    delay(1);
    return;
  }
  lastFrameAt = now;

  // draw cycle
  const uint32_t drawStartedUs = micros();
  backbuffer.fillScreen(lgfx::color888(0, 0, 0));

  const float sweepUpdateAngle = RadarSweep::Draw(backbuffer, sweepSettings, now);

  aircraftManager.Draw(
    backbuffer,
    sweepUpdateAngle,
    sweepSettings.enabled,
    sweepSettings.periodMs
  );

  const uint32_t pushStartedUs = micros();
  backbuffer.pushSprite(0, 0);
  const uint32_t frameEndedUs = micros();

  FrameTimer::Record(pushStartedUs - drawStartedUs,
                     frameEndedUs - pushStartedUs,
                     now,
                     sweepSettings.periodMs);
}
