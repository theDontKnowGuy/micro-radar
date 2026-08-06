#pragma once

#include <atomic>

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>

#include "FirmwareUpdater.h"

class ConfigurationWebServer {
public:
    // Which network the configuration page is being served over. There is one
    // page and one server either way; the mode only decides what the page can
    // honestly offer. In Setup the radar is its own access point with no route
    // to the internet, so place search, browser geolocation and the update
    // check are shown disabled rather than failing when pressed.
    enum class Mode { Station, Setup };

private:
    AsyncWebServer server;
    Preferences prefs;

    // Captive portal, Setup mode only: every name resolves to the radar so the
    // phone's own "sign in to this network" probe lands on the page. Polled by
    // PumpCaptivePortal() -- DNSServer has no asynchronous mode.
    DNSServer dns;
    Mode mode = Mode::Station;

    std::atomic<bool> restartRequested{false};

    // Borrowed, not owned; both live for the lifetime of the program. Only the
    // /update routes touch it, and they are registered by Initialise().
    FirmwareUpdater* firmwareUpdater = nullptr;

public:
    ConfigurationWebServer() : server(80), prefs() {}
    ConfigurationWebServer(int port) : server(port), prefs() {}

    // Binds port 80 for the rest of the boot. Call this once, in either mode --
    // the radar no longer hands the port between a setup portal and a
    // configuration server, which is what made first setup unreliable.
    //
    // The updater backs the "check for updates now" control on the page. It
    // does not need to have been started yet -- a check requested before then
    // is simply reported as idle.
    void Initialise(FirmwareUpdater& updater, Mode serveMode);

    // Creates the settings namespace and fills in any key that has never been
    // set. Initialise() does this too; call it first when something has to be
    // read before the mode is known -- the stored Wi-Fi credentials, which
    // decide the mode -- so the first read on a virgin device is not an NVS
    // "namespace not found" error on the serial log.
    void PrepareStorage() { EnsureDefaults(); }

    // Answers one pending captive-portal DNS query. No-op in Station mode.
    void PumpCaptivePortal();

    [[nodiscard]] String GetStoredString(const char* key);

    // Used by the one-shot import of credentials left in the Wi-Fi driver's own
    // NVS entry by earlier, WiFiManager-based firmware. Marks the import as
    // done, so a network forgotten from the page cannot come back on next boot.
    void SaveWiFiCredentials(const String& ssid, const String& password);

    // Saving settings needs a reboot, but the save handler runs on the AsyncTCP
    // task while the render loop is mid-frame with SPI DMA transfers in flight.
    // Restarting from there resets the chip with DMA still live, which corrupts
    // memory during early boot (InstructionFetchError, PC pointing into DRAM).
    // The handler raises this flag instead and the main loop reboots at a point
    // where the display is idle. It also lets the HTTP response finish sending.
    [[nodiscard]] bool RestartRequested() const { return restartRequested.load(); }

private:
    void EnsureDefaults();
};
