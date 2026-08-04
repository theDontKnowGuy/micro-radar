#pragma once

#include <atomic>

#include <ESPAsyncWebServer.h>
#include <Preferences.h>

class ConfigurationWebServer {
private:
    AsyncWebServer server;
    Preferences prefs;
    std::atomic<bool> restartRequested{false};

public:
    ConfigurationWebServer() : server(80), prefs() {}
    ConfigurationWebServer(int port) : server(port), prefs() {}

    void Initialise();
    [[nodiscard]] String GetStoredString(const char* key);

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
