#include <Arduino.h>
#include <WiFi.h>

#include "AircraftManager.h"
#include "ConfigurationWebServer.h"
#include "Diagnostics.h"
#include "FirmwareUpdater.h"
#include "FirmwareVersion.h"
#include "HttpRequestManager.h"
#include "LGFX.h"
#include "OpenSkyAuthTokenHandler.h"
#include "WiFiConnection.h"
#include "ui/AlignmentScreen.h"
#include "ui/BootScreen.h"
#include "ui/ConfigQr.h"
#include "ui/FrameTimer.h"
#include "ui/PanelTrim.h"
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

// Calibration mode: the alignment pattern in place of the radar. Read once, at
// the end of setup, like every other setting -- saving the page restarts the
// radar, so there is nothing to be gained by watching it change.
bool alignmentTestEnabled = false;

// Holds the radar on the configuration address until OpenSky credentials have
// been stored. There is no anonymous mode to fall back to any more: that
// allowance is counted per public address rather than per device, so a radar
// leaning on it spends an allowance shared with everything else behind the same
// router -- and gets a tenth of the refresh rate for it. Better to say so on
// the panel than to sweep once every three and a half minutes and look broken.
//
// Blocking, on the same reasoning as the setup portal: nothing that draws on
// the panel can usefully run, loop() is never reached, and the way out is the
// reboot that saving the page asks for. What does run is everything that had
// been started before the hold -- the configuration server on its own task, the
// diagnostics agent, and the update checker -- which is why this is called
// after all three rather than in place of them. A radar held here is the one
// that most needs to stay reachable: it is not showing anyone anything, so a
// release that fixes whatever put it here has to be able to land by itself.
[[noreturn]] void RunOpenSkySetupHold(const String& mdnsUrl)
{
  Serial.println("No OpenSky credentials stored - holding at the configuration screen");
  Diagnostics::Warn("no OpenSky credentials stored - waiting for configuration");

  // Same shape as the address screen below, with the reason on top: whoever is
  // looking at this has already set the radar up once, so the line that matters
  // is the one saying what is still missing. A lambda because the update path
  // below has to put it back, and two copies of five lines would drift.
  const auto showHoldScreen = [&mdnsUrl] {
    ShowStatusScreen(tft,
                     "OpenSky key",
                     "needed",
                     mdnsUrl,
                     "or " + WiFi.localIP().toString(),
                     "v" FIRMWARE_VERSION);
  };

  showHoldScreen();

  while (!configServer.RestartRequested()) {
    Diagnostics::Poll();

    // The same handover loop() does, less the aircraft network task -- there is
    // no such task yet, which leaves the diagnostics agent as the only other
    // claim on the heap a TLS session needs. setup() runs on the Arduino loop
    // task, so Install()'s "from the render loop, not a background task" is
    // satisfied here for the same reason it is there.
    if (firmwareUpdater.UpdatePending()) {
      Diagnostics::PauseForUpdate();
      UpdateScreen::RunFirmwareUpdate(tft, firmwareUpdater);

      // Not reached in practice -- every path out of an install ends in a
      // reboot. Should one ever return, the address goes back up rather than
      // leaving a finished progress bar on the panel with nothing to say.
      showHoldScreen();
    }

    delay(2);
  }

  Serial.println("Settings saved - restarting");
  ShowStatusScreen(tft, "Settings saved", "Restarting");
  delay(1500);   // let the HTTP response finish going out
  tft.waitDMA(); // make sure the panel bus is idle -- see loop()
  ESP.restart();

  while (true)
    delay(1000); // ESP.restart() does not return, but it is not declared that way
}

void setup()
{
  Serial.begin(115200);
  //delay(5000); // avoids immediate serial output being cut off - uncomment if needed
  Serial.printf("Starting Micro-Radar by Artisian Electronics, v" FIRMWARE_VERSION "\n");
  Serial.printf("Reset reason: %d\n", esp_reset_reason());
  
  // initialise LGFX + screen
  tft.init();
  tft.invertDisplay(DISPLAY_INVERT); // see DisplayConfig.h
  // The backlight starts off (see LGFX.h). Clear the panel first so it is not
  // lit up on whatever random content its RAM powered on with.
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setBrightness(255);

  // Ahead of the first screen and so ahead of everything else, because the boot
  // logo is the earliest thing that has to come out square. That puts the read
  // of the settings before the Wi-Fi work below rather than after it; it only
  // opens Preferences, so nothing there minds going first.
  configServer.PrepareStorage();
  PanelTrim::Begin(configServer.GetStoredString("screen-trim").toFloat());

  BootScreen::Draw(tft);
  const unsigned long bootStartedAt = millis();

  // At 360x360 the backbuffer is 129,600 bytes. Keeping that in SRAM leaves too
  // little contiguous heap for the TLS handshake OpenSky needs -- requests fail
  // with "SSL - Memory allocation failed" -- so put it in PSRAM, which the
  // required N16R8 module has. The check only guards the allocation below from
  // asking for PSRAM on a board that has none.
  if (ESP.getPsramSize() > 0)
    backbuffer.setPsram(true);

  // Eight bits, and measured rather than assumed. Sixteen was tried, to give
  // the trim's anti-aliased text more than RGB332's eight levels of green to
  // land in, and it cost far more than the doubling of memory traffic it looks
  // like: drawing a frame went from 11ms to 49ms, against a push already pinned
  // at 27ms by the SPI clock. Thirteen frames a second to make small text
  // smoother is not a trade worth making, and the text got its own answer --
  // see PanelTrim, which now turns each run once when it changes rather than
  // once per frame.
  backbuffer.setColorDepth(8);

  // Remembered rather than only printed. This happens before Wi-Fi, so it is
  // too early for Diagnostics to report anything -- but a unit that cannot
  // allocate its backbuffer is a broken unit, and that is worth knowing about
  // from a distance rather than only from a console nobody is watching.
  bool backbufferFailed = false;
  if (!backbuffer.createSprite(SCREEN_SIZE, SCREEN_SIZE)) {
    backbufferFailed = true;
    Serial.printf("FATAL: could not allocate %dx%d backbuffer (%d bytes)\n",
                  SCREEN_SIZE, SCREEN_SIZE, SCREEN_SIZE * SCREEN_SIZE);
  }

  WiFi.mode(WIFI_STA);

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

  // As early as the network allows, so that anything going wrong from here on
  // is reportable. Silent unless an auth key has been stored -- see
  // Diagnostics.h for why that is the default and what it costs to change it.
  //
  // Nothing before this point can be reported: the agent installs its log hook
  // when it starts, so the panel bring-up and the Wi-Fi join are console-only.
  // The boot event it sends carries the version and the reset reason, which is
  // what most of that early stretch would have told you anyway.
  Diagnostics::Begin(configServer);

  if (backbufferFailed)
    Diagnostics::Error("backbuffer allocation failed (%d bytes) - display is dead",
                       SCREEN_SIZE * SCREEN_SIZE);

  // begin background server for configuration
  configServer.Initialise(firmwareUpdater, ConfigurationWebServer::Mode::Station);

  const String mdnsUrl = String("http://") + MdnsAddress;
  const String configurationUrl = String("http://") + WiFi.localIP().toString();
  Serial.print("Configure me at ");
  Serial.print(mdnsUrl);
  Serial.print(" or ");
  Serial.println(configurationUrl);

  // Start polling GitHub for new firmware. Needs Wi-Fi, so it goes after the
  // join; the checker delays its first look by a couple of minutes. Whether a
  // find installs itself or waits for the owner is read here rather than per
  // check -- saving the setting reboots the radar anyway.
  //
  // Ahead of the OpenSky hold below, and so ahead of the radar itself: a unit
  // that never gets its credentials would otherwise sit there unreachable by
  // any release, which is the one state where self-updating matters most.
  const String autoUpdateSetting = configServer.GetStoredString("auto-update");
  firmwareUpdater.SetAutoInstall(autoUpdateSetting != "false");
  firmwareUpdater.Initialise();

  // Ahead of the address screen rather than after it: the hold puts the same
  // address up with the reason above it, so showing the seven-second version
  // first would only delay the one that says what is wrong. Both fields are
  // required -- a client id without its secret authenticates nothing.
  if (configServer.GetStoredString("opensky-id").isEmpty() ||
      configServer.GetStoredString("opensky-secret").isEmpty())
    RunOpenSkySetupHold(mdnsUrl);

  // Once the radar starts updating itself the version on screen is the only way
  // to tell, at a glance, which build a given unit ended up on. Kept to the bare
  // number here -- the release date and description are on the configuration
  // page, where there is room for them.
  //
  // Both addresses are shown, name first: the name survives the router handing
  // out a different lease, but it needs an mDNS resolver at the other end, which
  // not every phone or network has. The IP is the fallback that always works.
  //
  // Scheme and address stay on one line each: a URL split across two reads as
  // two things to type. Both lines carry the scheme, including the fallback --
  // it costs a step of type size, and what it buys is that everything on this
  // screen is something that can be pasted into a browser as it stands.
  //
  // The name is what the QR code above them carries, because it is the address
  // that is still right after the next reboot. What the code cannot help with
  // is a phone that will not resolve a .local name, which is exactly what the
  // second line is for.
  ShowQrAddressScreen(tft,
                      "Configure at:",
                      mdnsUrl,
                      { ConfigQr::SizeWithin, ConfigQr::Draw },
                      "or " + configurationUrl,
                      "v" FIRMWARE_VERSION);

  // Keep the address visible long enough to read while the asynchronous
  // configuration server is already available.
  //
  // Pumped rather than slept through. This is the diagnostics agent's first ten
  // seconds of life and the only stretch of the boot where nothing calls
  // Diagnostics::Poll() -- which is what noticed, in the field, a radar that had
  // joined a router with no working internet behind it: the agent retried its
  // upload about a hundred times a second for the whole delay, and the storm was
  // still running when aircraftManager.Initialise() below asked the same heap for
  // a TLS session of its own. Polling here lets the agent stand itself down
  // within a fraction of a second instead, before it has anything to collide
  // with.
  const unsigned long addressShownAt = millis();
  while (millis() - addressShownAt < 10000) {
    Diagnostics::Poll();
    delay(2);
  }

  // initialise aircraft manager
  aircraftManager.Initialise();

  sweepSettings = RadarSweep::LoadSettings(configServer);
  alignmentTestEnabled = configServer.GetStoredString("alignment-test") == "true";
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
    // this board does not have the contiguous heap for two. The diagnostics
    // agent is a third, and stands down for the same reason.
    aircraftManager.SuspendNetworkTask();
    Diagnostics::PauseForUpdate();
    UpdateScreen::RunFirmwareUpdate(tft, firmwareUpdater);
    return;
  }

  WiFiConnection::Maintain();

  // Below the update check and above the alignment return, and both halves of
  // that matter.
  //
  // Below, because Poll() can restart a stood-down agent, and a restart on the
  // pass that is about to call PauseForUpdate() would pay for an agent init and
  // its multi-second teardown to accomplish nothing. Above, because a radar
  // left in alignment mode still has a network stack to protect, and that path
  // returns before it would reach anything further down.
  //
  // Reporting is the first thing to give way when it starts costing more than
  // it is worth -- see Diagnostics::Poll().
  Diagnostics::Poll();

  // Calibration. Below the restart and update checks above so the page can
  // still be saved and the radar still update itself out of this mode, and
  // painted once rather than per frame -- nothing on the pattern moves, and
  // holding the panel still is what makes it possible to sight along.
  if (alignmentTestEnabled) {
    static bool alignmentDrawn = false;
    if (!alignmentDrawn) {
      AlignmentScreen::Draw(tft);
      alignmentDrawn = true;
    }
    delay(10);
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

  const RadarSweep::SweepState sweep = RadarSweep::Compute(sweepSettings, now);

  // Background elements the beam is meant to sweep over -- range rings, the
  // clock -- go down first. The beam itself is drawn next, on top of them.
  // Aircraft and the rim labels go on top of that again, same as before: they
  // are the radar's targets and its furniture, not part of the display the
  // beam is illuminating.
  aircraftManager.DrawBackground(backbuffer, sweep.updateAngle, sweepSettings.enabled);
  RadarSweep::DrawFan(backbuffer, sweepSettings, sweep);

  aircraftManager.Draw(
    backbuffer,
    sweep.updateAngle,
    sweepSettings.enabled,
    sweepSettings.periodMs
  );

  const uint32_t pushStartedUs = micros();
  PanelTrim::PushFrame(backbuffer);
  const uint32_t frameEndedUs = micros();

  FrameTimer::Record(pushStartedUs - drawStartedUs,
                     frameEndedUs - pushStartedUs,
                     now,
                     sweepSettings.periodMs);
}
