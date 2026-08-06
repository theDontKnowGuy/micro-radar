#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "LGFX.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "AircraftManager.h"
#include "BootLogo.h"
#include "DrawHelpers.h"
#include "FirmwareUpdater.h"
#include "FirmwareVersion.h"

// Optional hard-coded Wi-Fi credentials. Leave both blank to skip pre-baking them and use the setup hotspot instead.
const char* preconfiguredWifiSsid = "";
const char* preconfiguredWifiPassword = "";

// The access point the radar puts up when it has no network to join. The
// configuration page is served over it, so setup and configuration are the same
// page rather than two.
constexpr const char* SetupHotspotName = "MicroRadar-Setup";

// How long to wait for a join before giving up on it. The Arduino core's own
// default is a silent 60s per attempt with no way out, which is long enough
// that a mistyped password looks like a hang.
constexpr unsigned long WifiConnectTimeoutMs = 20000;

// A router that is still coming back up after a power cut answers the second
// attempt when it did not answer the first. Two is enough to ride that out
// without leaving a radar with a genuinely wrong password sitting on a blank
// screen for a minute.
constexpr int WifiConnectAttempts = 2;

LGFX tft;
LGFX_Sprite backbuffer(&tft);

ConfigurationWebServer configServer;
HttpRequestManager http;
OpenSkyAuthTokenHandler authHandler(http);

AircraftManager aircraftManager(configServer, authHandler, http);
FirmwareUpdater firmwareUpdater;
bool renderRadarSweep = true;
unsigned long radarSweepPeriodMs = 5000;

// Set true to log per-frame draw/push timing, frame rate and free heap over
// serial every 2s. Handy when tuning the SPI clock or tracking down a
// frame-rate or memory regression; off by default so the log stays readable.
constexpr bool LOG_FRAME_TIMING = false;

void SecureWipe(void* data, size_t length)
{
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (length-- > 0)
    *bytes++ = 0;
}

// Centres up to four lines of status text on an otherwise blank panel. Every
// screen the radar shows before it starts sweeping looks like this.
void ShowStatusScreen(const String& first,
                      const String& second = "",
                      const String& third = "",
                      const String& fourth = "")
{
  const String lines[] = { first, second, third, fourth };
  int count = 0;
  for (const String& line : lines)
    count += line.isEmpty() ? 0 : 1;

  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));

  const int lineHeight = tft.fontHeight() + 10;
  int y = SCREEN_SIZE_DIV_2 - ((count - 1) * lineHeight) / 2;
  for (const String& line : lines) {
    if (line.isEmpty())
      continue;
    tft.drawCentreString(line, SCREEN_SIZE_DIV_2, y);
    y += lineHeight;
  }
}

// Firmware before the merged configuration page used WiFiManager, which left
// the credentials in the Wi-Fi driver's own NVS entry rather than in this
// firmware's settings. Without this, every radar updated over the air would
// come back up in setup mode and have to be re-entered by hand. Runs once: the
// import is recorded, so a network forgotten from the page stays forgotten.
void ImportStoredWiFiCredentials(String& ssid, String& password)
{
  if (configServer.GetStoredString("wifi-imported") == "true")
    return;

  wifi_config_t config = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0] != '\0') {
    char buffer[sizeof(config.sta.password) + 1] = {};

    memcpy(buffer, config.sta.ssid, sizeof(config.sta.ssid));
    buffer[sizeof(config.sta.ssid)] = '\0';
    ssid = buffer;

    memcpy(buffer, config.sta.password, sizeof(config.sta.password));
    buffer[sizeof(config.sta.password)] = '\0';
    password = buffer;

    SecureWipe(buffer, sizeof(buffer));
    Serial.printf("Imported Wi-Fi credentials for %s from the previous firmware\n", ssid.c_str());
  }

  configServer.SaveWiFiCredentials(ssid, password);
}

// No network to join: become one. The same configuration page is served over
// the radar's own access point, so the network, the radar centre, the OpenSky
// credentials and everything else are all set in a single submission -- which
// is what the reboot at the end of this is for.
[[noreturn]] void RunSetupPortal()
{
  Serial.println("No Wi-Fi connection - starting setup hotspot");

  // AP_STA rather than AP: the page's network list needs the station interface
  // to run a scan, which is not available with the access point alone.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SetupHotspotName);
  configServer.Initialise(firmwareUpdater, ConfigurationWebServer::Mode::Setup);

  ShowStatusScreen("- SETUP -",
                   "Connect to this WiFi hotspot:",
                   SetupHotspotName,
                   "then open " + WiFi.softAPIP().toString());

  // Blocking on purpose. Nothing else can usefully run without a network, and
  // loop() is never reached in this mode -- the only way out is the reboot
  // below, once the page has been saved. The captive-portal DNS is polled here
  // because DNSServer has no asynchronous mode of its own.
  while (!configServer.RestartRequested()) {
    configServer.PumpCaptivePortal();
    delay(2);
  }

  Serial.println("Setup saved - restarting");
  ShowStatusScreen("Settings saved", "Restarting");
  delay(1500);   // let the HTTP response finish going out
  tft.waitDMA(); // make sure the panel bus is idle -- see loop()
  ESP.restart();

  while (true)
    delay(1000); // ESP.restart() does not return, but it is not declared that way
}

void ShowBootLogo()
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

  runIndex = 0;
  runColor = 0;
  runRemaining = 0;
  SecureWipe(scanline, sizeof(scanline));

  delay(5000);
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.waitDMA();
}

// Takes the panel over for the duration of a firmware download. Called from the
// render loop, so no sprite push is in flight and drawing straight to the
// display is safe here even though it is not during normal operation.
void RunFirmwareUpdate()
{
  const FirmwareUpdater::Release release = firmwareUpdater.PendingRelease();
  Serial.printf("Updating firmware: %s -> %s\n", FIRMWARE_VERSION, release.version.c_str());

  // A firmware download and an OpenSky fetch both want a TLS session, and this
  // board does not have the contiguous heap for two.
  aircraftManager.SuspendNetworkTask();

  tft.waitDMA();
  tft.fillScreen(lgfx::color888(0, 0, 0));
  // Opaque text: the percentage below is redrawn in place, and without a
  // background colour each repaint would smear over the last one.
  tft.setTextColor(lgfx::color888(0, 255, 0), lgfx::color888(0, 0, 0));

  const int lineHeight = tft.fontHeight() + 10;
  tft.drawCentreString("Updating firmware", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight * 2);
  tft.drawCentreString(release.version, SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight);

  constexpr int BAR_HEIGHT = 16;
  const int barWidth = SCREEN_SIZE / 2;
  const int barX = (SCREEN_SIZE - barWidth) / 2;
  const int barY = SCREEN_SIZE_DIV_2 + lineHeight;
  tft.drawRect(barX, barY, barWidth, BAR_HEIGHT, lgfx::color888(0, 255, 0));

  // Redraw only when the whole-percent figure moves. The download is ~1.3MB in
  // small chunks, and repainting the bar on every one of them would put more
  // SPI traffic in the way of the transfer than the progress is worth.
  int lastPercent = -1;
  const bool installed = firmwareUpdater.Install(release,
    [&](size_t written, size_t total) {
      const int percent = static_cast<int>((written * 100) / total);
      if (percent == lastPercent)
        return;
      lastPercent = percent;

      const int fill = ((barWidth - 4) * percent) / 100;
      tft.fillRect(barX + 2, barY + 2, fill, BAR_HEIGHT - 4, lgfx::color888(0, 255, 0));
      tft.drawCentreString(String(percent) + "%   ", SCREEN_SIZE_DIV_2, barY + BAR_HEIGHT + 10);
    });

  if (installed) {
    tft.fillScreen(lgfx::color888(0, 0, 0));
    tft.drawCentreString("Restarting", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2);
    delay(1000);
    tft.waitDMA();
    ESP.restart();
  }

  // The running firmware is untouched -- a failed write only dirties the
  // inactive slot -- but the network worker was suspended above and cannot be
  // resumed safely, so the radar would sit on stale aircraft. Reboot instead:
  // it comes back on the current version and retries an hour later.
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.drawCentreString("Update failed", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight);
  tft.drawCentreString("Restarting", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2);
  delay(3000);
  tft.waitDMA();
  ESP.restart();
}

void setup()
{
  Serial.begin(115200);
  // delay(1000); // avoids immediate serial output being cut off - uncomment if needed

  // initialise LGFX + screen
  tft.init();
  tft.invertDisplay(DISPLAY_INVERT); // differs per panel, see DisplayConfig.h
  ShowBootLogo();

  // At 360x360 the backbuffer is 129,600 bytes. Keeping that in SRAM leaves too
  // little contiguous heap for the TLS handshake OpenSky needs -- requests fail
  // with "SSL - Memory allocation failed" -- so put it in PSRAM when the board
  // has any. Falls back to SRAM, which is fine for the 240x240 panel.
  if (ESP.getPsramSize() > 0)
    backbuffer.setPsram(true);

  backbuffer.setColorDepth(8);
  if (!backbuffer.createSprite(SCREEN_SIZE, SCREEN_SIZE)) {
    Serial.printf("FATAL: could not allocate %dx%d backbuffer (%d bytes)\n",
                  SCREEN_SIZE, SCREEN_SIZE, SCREEN_SIZE * SCREEN_SIZE);
  }

  // establish WiFi connection
  ShowStatusScreen("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);

  // The stored network decides which mode the page is served in, so the
  // settings have to be readable before that decision is made.
  configServer.PrepareStorage();

  String ssid = configServer.GetStoredString("wifi-ssid");
  String password = configServer.GetStoredString("wifi-pass");

  if (strlen(preconfiguredWifiSsid) > 0) {
    ssid = preconfiguredWifiSsid;
    password = preconfiguredWifiPassword;
  }
  else if (ssid.isEmpty()) {
    ImportStoredWiFiCredentials(ssid, password);
  }

  bool connected = false;
  for (int attempt = 0; !connected && !ssid.isEmpty() && attempt < WifiConnectAttempts; ++attempt) {
    if (attempt > 0)
      ShowStatusScreen("Connecting to WiFi...", ssid, "Retrying");

    WiFi.begin(ssid.c_str(), password.c_str());
    connected = WiFi.waitForConnectResult(WifiConnectTimeoutMs) == WL_CONNECTED;
    if (!connected)
      WiFi.disconnect();
  }

  // Nothing stored, or the stored network would not have us. Either way the
  // radar has no way to be configured except by serving the page itself; this
  // does not return.
  if (!connected)
    RunSetupPortal();

  // begin background server for configuration
  configServer.Initialise(firmwareUpdater, ConfigurationWebServer::Mode::Station);

  const String configurationUrl = String("http://") + WiFi.localIP().toString();
  Serial.print("Configure me at ");
  Serial.println(configurationUrl);

  // Once the radar starts updating itself the version on screen is the only way
  // to tell, at a glance, which build a given unit ended up on. Kept to the bare
  // number here -- the release date and description are on the configuration
  // page, where there is room for them.
  ShowStatusScreen("Configure me at", configurationUrl, "v" FIRMWARE_VERSION);

  // Keep the address visible long enough to read while the asynchronous
  // configuration server is already available.
  delay(4000);

  // initialise aircraft manager
  aircraftManager.Initialise();

  // Start polling GitHub for new firmware. Needs Wi-Fi, so it goes after
  // autoConnect; the checker delays its first look by a couple of minutes.
  // Whether a find installs itself or waits for the owner is read here rather
  // than per check -- saving the setting reboots the radar anyway.
  const String autoUpdateSetting = configServer.GetStoredString("auto-update");
  firmwareUpdater.SetAutoInstall(autoUpdateSetting != "false");
  firmwareUpdater.Initialise();

  // Read display configuration once. Reading Preferences in every frame
  // causes visible NVS-related frame-time spikes.
  const String scanlineSetting = configServer.GetStoredString("scanline");
  renderRadarSweep = scanlineSetting.isEmpty() || scanlineSetting == "true";
  long sweepPeriodSeconds = configServer.GetStoredString("sweep-period").toInt();
  if (sweepPeriodSeconds < 2 || sweepPeriodSeconds > 60)
    sweepPeriodSeconds = 5;
  radarSweepPeriodMs = static_cast<unsigned long>(sweepPeriodSeconds) * 1000UL;
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
    RunFirmwareUpdate();
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

  float sweepAngle = 0.0f;
  float sweepUpdateAngle = 0.0f;
  if (renderRadarSweep) {
    constexpr int SWEEP_THICKNESS = 20;
    constexpr int SWEEP_SPACING = 5;
    sweepAngle = (now % radarSweepPeriodMs) * (TWO_PI / radarSweepPeriodMs);
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
    sweepUpdateAngle = sweepAngle + SWEEP_LEADING_EDGE_OFFSET;
    if (sweepUpdateAngle >= TWO_PI)
      sweepUpdateAngle -= TWO_PI;
  }

  aircraftManager.Draw(
    backbuffer,
    sweepUpdateAngle,
    renderRadarSweep,
    radarSweepPeriodMs
  );

  const uint32_t pushStartedUs = micros();
  backbuffer.pushSprite(0, 0);
  const uint32_t frameEndedUs = micros();

  // The sprite push is SCREEN_SIZE^2 * 2 bytes over SPI and dominates the frame,
  // so the SPI clock sets the frame rate directly. Reported as degrees-of-sweep
  // per frame because that is what actually reads as smooth or jumpy.
  static uint32_t frameCount = 0;
  static uint32_t drawUsTotal = 0;
  static uint32_t pushUsTotal = 0;
  static unsigned long lastTimingReportAt = 0;

  drawUsTotal += pushStartedUs - drawStartedUs;
  pushUsTotal += frameEndedUs - pushStartedUs;
  frameCount++;

  if (LOG_FRAME_TIMING && now - lastTimingReportAt >= 2000) {
    lastTimingReportAt = now;
    const float drawMs = (drawUsTotal / 1000.0f) / frameCount;
    const float pushMs = (pushUsTotal / 1000.0f) / frameCount;
    const float totalMs = drawMs + pushMs;
    Serial.printf(
      "frame: draw %.1fms  push %.1fms  total %.1fms  %.1f fps  %.1f deg/frame"
      "  heap %u (largest %u)\n",
      drawMs, pushMs, totalMs,
      totalMs > 0.0f ? 1000.0f / totalMs : 0.0f,
      360.0f * (totalMs / static_cast<float>(radarSweepPeriodMs)),
      ESP.getFreeHeap(), ESP.getMaxAllocHeap()
    );
    frameCount = 0;
    drawUsTotal = 0;
    pushUsTotal = 0;
  }
}
