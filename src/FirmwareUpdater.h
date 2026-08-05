#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <functional>

// Pull-based OTA. A low-priority background task fetches a small JSON manifest
// from the project's GitHub releases once an hour; if it advertises a version
// newer than FIRMWARE_VERSION for this board's build key, it records the
// details and raises a flag. The main loop notices the flag and calls Install()
// at a point where the panel is idle.
//
// The download deliberately does NOT happen on the background task. Flashing
// from there would leave the render loop running -- pushing sprites over SPI
// DMA -- while the chip rewrites flash and then resets, which is the same
// hazard ConfigurationWebServer::RestartRequested() exists to avoid. Doing it
// from the main loop also means the progress callback can draw straight to the
// panel without touching the display from two tasks at once.
//
// TLS is pinned to the two roots in UpdateRootCAs.h rather than trusting any
// CA, because this channel can execute arbitrary code on the device.
class FirmwareUpdater
{
public:
    struct Release {
        String version;
        String notes;
        String url;
        String md5;
        size_t size = 0;
    };

    // Invoked repeatedly during the flash with bytes-written and total. Runs on
    // whichever task called Install(), i.e. the render loop, so drawing from it
    // is safe.
    using ProgressCallback = std::function<void(size_t written, size_t total)>;

    FirmwareUpdater() = default;
    ~FirmwareUpdater() = default;

    // Starts the hourly checker. Call once Wi-Fi is up.
    void Initialise();

    // True once a newer release has been found and is waiting to be installed.
    [[nodiscard]] bool UpdatePending() const { return updatePending.load(); }

    // Details of the release UpdatePending() is reporting.
    [[nodiscard]] Release PendingRelease() const;

    // Downloads the image into the inactive OTA slot and verifies it against
    // the manifest's MD5 before switching the boot slot over. Returns false and
    // leaves the running firmware untouched on any failure; the caller should
    // carry on rendering. On success the caller is expected to reboot -- this
    // does not restart the chip itself, so the panel can be brought to a safe
    // state first.
    //
    // Must be called from the render loop, not from a background task.
    bool Install(const Release& release, const ProgressCallback& onProgress);

    // Last failure, for the serial log. Empty when the last operation worked.
    [[nodiscard]] String LastError() const;

private:
    struct Version {
        long major = 0;
        long minor = 0;
        long patch = 0;
        bool valid = false;
    };

    std::atomic<bool> updatePending{false};
    SemaphoreHandle_t stateMutex = nullptr;
    TaskHandle_t checkTaskHandle = nullptr;
    Release pendingRelease;
    String lastError;

    void CheckTaskLoop();
    static void CheckTaskEntry(void* context);

    // Fetches and parses the manifest, populating pendingRelease and raising
    // updatePending when it advertises something newer for this build.
    void CheckForUpdate();

    // Common exit for every failure path in Install(). Always returns false.
    bool Abandon();

    void SetLastError(const String& message);

    [[nodiscard]] static Version ParseVersion(const String& raw);
    [[nodiscard]] static bool IsNewer(const Version& candidate, const Version& current);
};
