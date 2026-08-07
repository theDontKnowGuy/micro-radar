#include "WiFiConnection.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "ConfigurationWebServer.h"
#include "FirmwareUpdater.h"
#include "ui/StatusScreen.h"

// Optional hard-coded Wi-Fi credentials. Leave both blank to skip pre-baking them and use the setup hotspot instead.
const char* preconfiguredWifiSsid = "";
const char* preconfiguredWifiPassword = "";

namespace WiFiConnection {
namespace {

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

// Found by BeginJoin, waited on by AwaitJoin, and used again by the retry it
// makes -- which is why they outlive the call that reads them.
String storedSsid;
String storedPassword;

void SecureWipe(void* data, size_t length)
{
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (length-- > 0)
    *bytes++ = 0;
}

// Firmware before the merged configuration page used WiFiManager, which left
// the credentials in the Wi-Fi driver's own NVS entry rather than in this
// firmware's settings. Without this, every radar updated over the air would
// come back up in setup mode and have to be re-entered by hand. Runs once: the
// import is recorded, so a network forgotten from the page stays forgotten.
void ImportStoredWiFiCredentials(ConfigurationWebServer& configServer, String& ssid, String& password)
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

}

void BeginJoin(ConfigurationWebServer& configServer)
{
  storedSsid = configServer.GetStoredString("wifi-ssid");
  storedPassword = configServer.GetStoredString("wifi-pass");

  if (strlen(preconfiguredWifiSsid) > 0) {
    storedSsid = preconfiguredWifiSsid;
    storedPassword = preconfiguredWifiPassword;
  }
  else if (storedSsid.isEmpty()) {
    ImportStoredWiFiCredentials(configServer, storedSsid, storedPassword);
  }

  // WiFi.begin() only kicks the driver off -- the waiting is all in AwaitJoin.
  if (!storedSsid.isEmpty())
    WiFi.begin(storedSsid.c_str(), storedPassword.c_str());
}

bool AwaitJoin(LGFX& tft, unsigned long bootStartedAt)
{
  const bool haveCredentials = !storedSsid.isEmpty();
  bool connected = haveCredentials && WiFi.status() == WL_CONNECTED;

  // The join is usually done by the time the logo comes down. If it is not --
  // a slow router, a wrong password, an attempt that needs repeating -- this is
  // where it gets said out loud, and the first attempt is given the rest of its
  // timeout counted from when it actually started.
  for (int attempt = 0; !connected && haveCredentials && attempt < WifiConnectAttempts; ++attempt) {
    if (attempt == 0) {
      const unsigned long spent = millis() - bootStartedAt;
      if (spent >= WifiConnectTimeoutMs) {
        WiFi.disconnect();
        continue;
      }

      ShowStatusScreen(tft, "Connecting to WiFi...", storedSsid);
      connected = WiFi.waitForConnectResult(WifiConnectTimeoutMs - spent) == WL_CONNECTED;
    }
    else {
      ShowStatusScreen(tft, "Connecting to WiFi...", storedSsid, "Retrying");
      WiFi.begin(storedSsid.c_str(), storedPassword.c_str());
      connected = WiFi.waitForConnectResult(WifiConnectTimeoutMs) == WL_CONNECTED;
    }

    if (!connected)
      WiFi.disconnect();
  }

  // Nothing past here needs them, and neither the password nor the SSID has any
  // business sitting on the heap for the rest of the run: the driver has its
  // own copy, and the portal below does not join anything.
  storedSsid = String();
  storedPassword = String();

  return connected;
}

void RunSetupPortal(LGFX& tft, ConfigurationWebServer& configServer, FirmwareUpdater& updater)
{
  Serial.println("No Wi-Fi connection - starting setup hotspot");

  // AP_STA rather than AP: the page's network list needs the station interface
  // to run a scan, which is not available with the access point alone.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SetupHotspotName);
  configServer.Initialise(updater, ConfigurationWebServer::Mode::Setup);

  // Kept to short lines on purpose: ShowStatusScreen sizes the type to the
  // longest one, and this is the screen someone reads with the radar at arm's
  // length while they hunt for the hotspot on their phone.
  ShowStatusScreen(tft,
                   "- SETUP -",
                   "Connect to WiFi:",
                   SetupHotspotName,
                   "then open",
                   WiFi.softAPIP().toString());

  // Blocking on purpose. Nothing else can usefully run without a network, and
  // loop() is never reached in this mode -- the only way out is the reboot
  // below, once the page has been saved. The captive-portal DNS is polled here
  // because DNSServer has no asynchronous mode of its own.
  while (!configServer.RestartRequested()) {
    configServer.PumpCaptivePortal();
    delay(2);
  }

  Serial.println("Setup saved - restarting");
  ShowStatusScreen(tft, "Settings saved", "Restarting");
  delay(1500);   // let the HTTP response finish going out
  tft.waitDMA(); // make sure the panel bus is idle -- see loop()
  ESP.restart();

  while (true)
    delay(1000); // ESP.restart() does not return, but it is not declared that way
}

}
