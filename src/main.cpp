#include <Arduino.h>
#include <WiFiManager.h>

#include "LGFX.h"
#include "WiFiManagerHelpers.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "AircraftManager.h"
#include "DrawHelpers.h"

// Optional hard-coded Wi-Fi credentials. Leave both blank to skip pre-baking them and use the setup hotspot instead.
const char* preconfiguredWifiSsid = "";
const char* preconfiguredWifiPassword = "";

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

LGFX tft;
LGFX_Sprite backbuffer(&tft);

WiFiManager wm;
ConfigurationWebServer configServer;
HttpRequestManager http;
OpenSkyAuthTokenHandler authHandler(http);

AircraftManager aircraftManager(configServer, authHandler, http);
bool renderRadarSweep = true;
unsigned long radarSweepPeriodMs = 5000;

void setup()
{
  Serial.begin(115200);
  // delay(1000); // avoids immediate serial output being cut off - uncomment if needed

  // initialise LGFX + screen
  tft.init();
  tft.invertDisplay(true);

  backbuffer.setColorDepth(8);
  backbuffer.createSprite(SCREEN_SIZE, SCREEN_SIZE);

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
  backbuffer.pushSprite(0, 0);
}
