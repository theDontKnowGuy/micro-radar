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

    // What the updater is doing, for the "check now" control on the
    // configuration page. Idle means no check has run yet.
    enum class Status : uint8_t {
        Idle,
        Checking,
        UpToDate,
        UpdateFound,
        Downloading,
        Installed,
        Failed
    };

    struct StatusSnapshot {
        Status status = Status::Idle;
        String message;

        // True while a release has been found but manual mode is holding it
        // back. The configuration page turns this into its "Install now"
        // control, and stops polling on it -- nothing more will happen on the
        // radar's own initiative.
        bool awaitingConfirmation = false;
    };

    // Invoked repeatedly during the flash with bytes-written and total. Runs on
    // whichever task called Install(), i.e. the render loop, so drawing from it
    // is safe.
    using ProgressCallback = std::function<void(size_t written, size_t total)>;

    FirmwareUpdater() = default;
    ~FirmwareUpdater() = default;

    // Chooses what happens when the hourly check finds something: install it
    // straight away, or hold it until someone presses the configuration page's
    // "Install now". Set from the stored setting before Initialise(); a later
    // change reaches the updater through the reboot that saving settings does.
    void SetAutoInstall(bool enabled) { autoInstall.store(enabled); }

    // Starts the hourly checker. Call once Wi-Fi is up.
    void Initialise();

    // True once a release has been accepted for installation -- automatically,
    // or by RequestInstallNow(). The render loop watches this.
    [[nodiscard]] bool UpdatePending() const { return updatePending.load(); }

    // Accepts the release the last check found, for manual mode. Returns false
    // when nothing is waiting, which is what a stale page button looks like.
    bool RequestInstallNow();

    // True while a release is known about and manual mode is holding it back.
    // Drives both the page's "Install now" control and the radar's on-screen
    // notice -- without one of those, "ask me first" never gets round to asking.
    [[nodiscard]] bool AwaitingConfirmation() const
    {
        return updateAvailable.load() && !autoInstall.load() && !updatePending.load();
    }

    // Details of the release UpdatePending() is reporting.
    [[nodiscard]] Release PendingRelease() const;

    // Wakes the hourly checker so it looks now instead of at its next tick.
    // Called from the web server's task, so it only posts a notification --
    // the fetch itself still happens on the checker task, which is the one with
    // the stack for a TLS handshake.
    void RequestCheckNow();

    // Current activity, for the configuration page to poll.
    [[nodiscard]] StatusSnapshot CurrentStatus() const;

    // Stable machine-readable name, used as the JSON status value.
    [[nodiscard]] static const char* StatusName(Status status);

    // Downloads the image into the inactive OTA slot and verifies it against
    // the manifest's MD5 before switching the boot slot over. Returns false and
    // leaves the running firmware untouched on any failure; the caller should
    // carry on rendering. On success the caller is expected to reboot -- this
    // does not restart the chip itself, so the panel can be brought to a safe
    // state first.
    //
    // If the Arduino Update library manages the MD5 but cannot activate the
    // slot, the image is checked once more through the memory-mapped read path
    // and the boot slot is moved by hand -- see ActivateSlotVerifiedByMapping().
    //
    // Must be called from the render loop, not from a background task.
    bool Install(const Release& release, const ProgressCallback& onProgress);

private:
    struct Version {
        long major = 0;
        long minor = 0;
        long patch = 0;
        bool valid = false;
    };

    std::atomic<bool> updatePending{false};

    // Separate from updatePending: a release can be known about without having
    // been accepted for installation, which is the whole of manual mode.
    std::atomic<bool> updateAvailable{false};
    std::atomic<bool> autoInstall{true};

    SemaphoreHandle_t stateMutex = nullptr;
    TaskHandle_t checkTaskHandle = nullptr;
    Release pendingRelease;
    Status status = Status::Idle;
    String statusMessage;

    void CheckTaskLoop();
    static void CheckTaskEntry(void* context);

    // Fetches and parses the manifest, populating pendingRelease and raising
    // updatePending when it advertises something newer for this build.
    void CheckForUpdate();

    // Common exit for every failure path in Install(). Always returns false.
    bool Abandon();

    // Last resort when Update.end() reports UPDATE_ERROR_ACTIVATE.
    //
    // That error means the MD5 matched -- Update checks the digest before it
    // tries to activate anything -- and that esp_ota_set_boot_partition()
    // refused. It refuses by re-reading the freshly written slot through
    // esp_flash_read() and hashing it, and at least one unit in the field has a
    // flash chip that returns the first 32 bytes of any longer read and then
    // repeats them. On that board the hash is garbage while the image itself is
    // byte-perfect, so a good release can never be activated.
    //
    // This repeats the same verification the API would have done, reading
    // through spi_flash_mmap instead: the cache path, which reads correctly on
    // that unit and is what the bootloader itself will use at the next boot.
    // Only if the image passes its own appended SHA-256 is otadata rewritten by
    // hand to point at the slot.
    //
    // Nothing here weakens the chain. The manifest MD5 has already matched, the
    // image is checked against the digest the build baked into it, and the
    // bootloader verifies it independently before running it -- so a genuinely
    // corrupt image still cannot boot. Returns false on any doubt.
    bool ActivateSlotVerifiedByMapping();

    // Reports the release currently held in pendingRelease, wording it for
    // whichever mode is in force.
    void AnnouncePendingRelease();

    // Records what the updater is doing. Failures are logged as they are set,
    // so every error reaches the serial log exactly once.
    void SetStatus(Status newStatus, const String& message);

    [[nodiscard]] static Version ParseVersion(const String& raw);
    [[nodiscard]] static bool IsNewer(const Version& candidate, const Version& current);
};
