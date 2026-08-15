#include "WiFiConnection.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "ConfigurationWebServer.h"
#include "FirmwareUpdater.h"
#include "ui/StatusScreen.h"
#include "ui/WifiSetupQr.h"

// Optional hard-coded Wi-Fi credentials. Leave both blank to skip pre-baking them and use the setup hotspot instead.
const char* preconfiguredWifiSsid = "";
const char* preconfiguredWifiPassword = "";

namespace WiFiConnection {
namespace {

// Found by BeginJoin and waited on by everything after it, which is why they
// outlive the call that reads them.
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

  if (storedSsid.isEmpty())
    return;

  // The retrying is the driver's job, not this module's. Its disconnect handler
  // re-issues the association itself for every transient failure -- no AP found,
  // handshake timeout, a router mid-reboot -- which covers the cases a manual
  // retry here would have been for, and covers them sooner. What it will not do
  // is come back from a disconnect this firmware asked for, so nothing below
  // calls WiFi.disconnect() until the join has been given up on for good.
  WiFi.setAutoReconnect(true);

  // The name the radar gives the router's DHCP server, kept the same as the
  // mDNS name so a router that publishes its leases under .local and the
  // radar's own responder do not disagree about what this device is called.
  // Has to precede WiFi.begin() -- the hostname goes out with the DHCP request.
  WiFi.setHostname(MdnsHostname);

  // Only kicks the driver off; the waiting happens under the boot logo.
  WiFi.begin(storedSsid.c_str(), storedPassword.c_str());
}

bool JoinPending()
{
  return !storedSsid.isEmpty() && WiFi.status() != WL_CONNECTED;
}

bool FinishJoin()
{
  const bool connected = !storedSsid.isEmpty() && WiFi.status() == WL_CONNECTED;

  if (!connected) {
    // Give up for good. This is the one disconnect that is wanted: it stops the
    // driver reconnecting underneath the setup portal, where the station
    // interface is needed for scanning rather than for joining.
    WiFi.disconnect();

    // Only worth saying when there was something to join -- a radar that has
    // never been set up has not failed at anything.
    if (!storedSsid.isEmpty())
      Serial.printf("Could not join %s within %lums, status %d\n",
                    storedSsid.c_str(), JoinTimeoutMs, WiFi.status());
  }

  // Neither the password nor the SSID has any business sitting on the heap for
  // the rest of the run -- the driver has its own copy of the pair it is using,
  // and nothing past here joins anything. Wiped rather than just released, so
  // the password does not linger in whatever the heap hands out next.
  SecureWipe(const_cast<char*>(storedPassword.c_str()), storedPassword.length());
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

  // The code is the way anyone with a phone in their hand should join --
  // scanning it does what typing the SSID in by hand does, minus the chance of
  // a typo. The name stays on screen under it regardless, both as the fallback
  // for a phone whose camera app will not offer to join a network from a code
  // and as confirmation, while the code is in frame, that it is not going to
  // ask for a password once tapped: the network really is open.
  //
  // The heading carries the instruction rather than the old "- SETUP -" label:
  // ShowQrAddressScreen has one heading line, not two, and someone who has never
  // seen this radar before needs to be told to connect more than they need to
  // be told they are in a mode called setup.
  ShowQrAddressScreen(tft,
                      "Connect to WiFi:",
                      SetupHotspotName,
                      { WifiSetupQr::SizeWithin, WifiSetupQr::Draw },
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
