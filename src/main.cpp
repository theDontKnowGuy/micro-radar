#include <Arduino.h>
#include <WiFiManager.h>

#include "LGFX.h"
#include "WiFiManagerHelpers.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "AircraftManager.h"
#include "BootLogo.h"
#include "DrawHelpers.h"

// Optional hard-coded Wi-Fi credentials. Leave both blank to skip pre-baking them and use the setup hotspot instead.
const char* preconfiguredWifiSsid = "";
const char* preconfiguredWifiPassword = "";

LGFX tft;
LGFX_Sprite backbuffer(&tft);

WiFiManager wm;
ConfigurationWebServer configServer;
HttpRequestManager http;
OpenSkyAuthTokenHandler authHandler(http);

AircraftManager aircraftManager(configServer, authHandler, http);
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
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));
  tft.drawCentreString("Connecting to WiFi...", SCREEN_SIZE / 2, SCREEN_SIZE / 2);

  WiFiManagerHelpers::ConfigureWiFiManager(wm, tft);

  if (strlen(preconfiguredWifiSsid) > 0) {
    WiFi.begin(preconfiguredWifiSsid, preconfiguredWifiPassword);
    WiFi.waitForConnectResult();
  }

  wm.autoConnect(WiFiManagerHelpers::WiFiManagerName);

  // begin background server for configuration
  configServer.Initialise();

  if (WiFi.status() == WL_CONNECTED) {
    const String configurationUrl = String("http://") + WiFi.localIP().toString();
    Serial.print("Configure me at ");
    Serial.println(configurationUrl);

    tft.fillScreen(lgfx::color888(0, 0, 0));
    tft.setTextColor(lgfx::color888(0, 255, 0));
    const int lineHeight = tft.fontHeight() + 10;
    tft.drawCentreString(
      "Configure me at",
      SCREEN_SIZE / 2,
      SCREEN_SIZE / 2 - lineHeight
    );
    tft.drawCentreString(
      configurationUrl,
      SCREEN_SIZE / 2,
      SCREEN_SIZE / 2
    );

    // Keep the address visible long enough to read while the asynchronous
    // configuration server is already available.
    delay(4000);
  }

  // initialise aircraft manager
  aircraftManager.Initialise();

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
