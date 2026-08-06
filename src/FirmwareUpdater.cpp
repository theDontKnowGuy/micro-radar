#include "FirmwareUpdater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>

#include "FirmwareVersion.h"
#include "UpdateRootCAs.h"

namespace {

// Where the radar looks for new firmware. GitHub keeps this path pointing at
// the newest published (non-draft, non-prerelease) release, so the URL never
// changes as versions come and go. It answers with a 302 to
// objects.githubusercontent.com, which is why redirects are followed below and
// why UpdateRootCAs.h pins two roots rather than one.
//
// Change the owner/repo here if you fork the project.
constexpr char MANIFEST_URL[] =
    "https://github.com/thedontknowguy/micro-radar/releases/latest/download/manifest.json";

// Leave the radar alone for a couple of minutes after boot. The first frames
// are also when Wi-Fi is settling and the aircraft manager is doing its initial
// fetch, and there is nothing to be gained by competing with that.
constexpr uint32_t FIRST_CHECK_DELAY_MS = 2 * 60 * 1000;
constexpr uint32_t CHECK_INTERVAL_MS = 60 * 60 * 1000;

// The manifest is a few hundred bytes; anything much larger is not ours.
constexpr int MANIFEST_MAX_BYTES = 8192;

constexpr uint32_t HTTP_TIMEOUT_MS = 15000;

// Give up on a download that goes quiet for this long. Without it a half-open
// TCP connection would hold the render loop on the progress screen forever.
constexpr uint32_t DOWNLOAD_STALL_TIMEOUT_MS = 20000;

// One TCP segment is around 1460 bytes, so this drains a couple of segments per
// pass without putting a large buffer on the loop task's stack.
constexpr size_t DOWNLOAD_CHUNK_BYTES = 2048;

// TLS plus JSON on one stack. The handshake is the expensive part.
constexpr uint32_t CHECK_TASK_STACK = 12288;

}  // namespace

void FirmwareUpdater::Initialise()
{
    stateMutex = xSemaphoreCreateMutex();
    if (stateMutex == nullptr) {
        Serial.println("[ERROR] Could not create updater mutex; auto-update disabled");
        return;
    }

    Serial.printf("[OTA] Running %s (build %s)\n", FIRMWARE_VERSION, FIRMWARE_BUILD);

    // Priority 1 matches the aircraft network task: this must never outrank the
    // render loop, and it spends almost all of its life blocked in vTaskDelay.
    const BaseType_t taskCreated = xTaskCreatePinnedToCore(
        CheckTaskEntry,
        "radar-update",
        CHECK_TASK_STACK,
        this,
        1,
        &checkTaskHandle,
        0
    );
    if (taskCreated != pdPASS) {
        checkTaskHandle = nullptr;
        Serial.println("[ERROR] Could not create update task; auto-update disabled");
    }
}

void FirmwareUpdater::CheckTaskEntry(void* context)
{
    static_cast<FirmwareUpdater*>(context)->CheckTaskLoop();
}

void FirmwareUpdater::CheckTaskLoop()
{
    // Waiting on a notification rather than sleeping outright is what lets the
    // "check now" control on the configuration page jump the queue: the wait
    // ends either when someone asks or when the interval runs out.
    uint32_t waitMs = FIRST_CHECK_DELAY_MS;

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
        waitMs = CHECK_INTERVAL_MS;

        // Nothing raises the flag but this task, and nothing clears it but a
        // successful install followed by a reboot. Once something is pending
        // there is no point looking again -- keep the radio quiet and let the
        // main loop get to it.
        if (updatePending.load())
            continue;

        // Manual mode parks here between the release being found and the owner
        // pressing Install. Fetching the manifest again would return the same
        // answer, so just restate it -- a "check now" pressed in the meantime
        // gets a reply instead of leaving the page polling a stale "checking".
        if (updateAvailable.load()) {
            AnnouncePendingRelease();
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) {
            SetStatus(Status::Failed, "not connected to Wi-Fi");
            continue;
        }

        CheckForUpdate();
    }
}

void FirmwareUpdater::RequestCheckNow()
{
    if (checkTaskHandle == nullptr)
        return;

    // Set here rather than on the checker task so the page shows "checking"
    // from the moment the request is accepted, not whenever the task is
    // scheduled next.
    SetStatus(Status::Checking, "Checking for updates");
    xTaskNotifyGive(checkTaskHandle);
}

void FirmwareUpdater::CheckForUpdate()
{
    WiFiClientSecure client;
    client.setCACert(UpdateRootCAs::GitHubRoots);
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);  // seconds, unlike HTTPClient's

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);

    if (!http.begin(client, MANIFEST_URL)) {
        SetStatus(Status::Failed, "could not open manifest connection");
        return;
    }

    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        // A 404 here is the normal state of a repo that has no releases yet, so
        // this is a warning rather than an error.
        SetStatus(Status::Failed, "manifest fetch returned HTTP " + String(status));
        http.end();
        return;
    }

    if (http.getSize() > MANIFEST_MAX_BYTES) {
        SetStatus(Status::Failed, "manifest is implausibly large (" + String(http.getSize()) + " bytes)");
        http.end();
        return;
    }

    const String payload = http.getString();
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        SetStatus(Status::Failed, String("manifest parse failed: ") + error.c_str());
        return;
    }

    // Read every field with an explicit default. A missing key then reads as
    // empty rather than as whatever the library coerces null into, so the
    // validation below is the only thing deciding what counts as usable.
    const String offeredVersion = doc["version"] | "";

    const Version current = ParseVersion(FIRMWARE_VERSION);
    const Version offered = ParseVersion(offeredVersion);
    if (!offered.valid) {
        SetStatus(Status::Failed, "manifest has no usable version field");
        return;
    }

    if (!IsNewer(offered, current)) {
        Serial.printf("[OTA] Up to date (running %s, latest %s)\n",
                      FIRMWARE_VERSION, offeredVersion.c_str());
        SetStatus(Status::UpToDate,
                  "Up to date - " FIRMWARE_VERSION " is the latest release");
        return;
    }

    // One manifest covers every panel variant. Flashing a 360x360 GC9B72 image
    // onto a 240x240 GC9A01 board would produce a device that boots to a broken
    // display and can only be recovered over USB, so a missing entry for this
    // build is a hard stop rather than something to guess around.
    JsonVariant build = doc["builds"][FIRMWARE_BUILD];
    if (!build.is<JsonObject>()) {
        SetStatus(Status::Failed, "release " + offeredVersion + " has no build for " + FIRMWARE_BUILD);
        return;
    }

    Release release;
    release.version = offeredVersion;
    release.notes = doc["notes"] | "";
    release.url = build["url"] | "";
    release.md5 = build["md5"] | "";
    release.size = build["size"] | 0U;

    if (release.url.isEmpty() || release.md5.isEmpty()) {
        SetStatus(Status::Failed, "build entry is missing url or md5");
        return;
    }

    // Everything is fetched over a pinned-CA connection, but the MD5 is still
    // worth insisting on: it is what makes a truncated or corrupted download
    // fail before it can be marked bootable.
    if (release.md5.length() != 32) {
        SetStatus(Status::Failed, "build entry md5 is not a 32-character digest");
        return;
    }

    // Defence in depth. The manifest already arrived over a pinned connection,
    // so a hostile URL here would need GitHub itself to be serving it -- but
    // pinning only buys anything if the download also goes somewhere those two
    // roots cover. Refuse anything that would drop to plain HTTP or leave.
    if (!release.url.startsWith("https://github.com/") &&
        !release.url.startsWith("https://objects.githubusercontent.com/")) {
        SetStatus(Status::Failed, "refusing firmware url outside GitHub: " + release.url);
        return;
    }

    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        pendingRelease = release;
        xSemaphoreGive(stateMutex);
    }

    updateAvailable.store(true);

    // Status before the flag: the render loop starts installing the moment it
    // sees updatePending, so setting it second means the page can never poll a
    // download that has not been announced.
    AnnouncePendingRelease();
    if (autoInstall.load())
        updatePending.store(true);

    Serial.printf("[OTA] Update available: %s -> %s (%u bytes)\n",
                  FIRMWARE_VERSION, release.version.c_str(),
                  static_cast<unsigned>(release.size));
    if (!release.notes.isEmpty())
        Serial.printf("[OTA] %s\n", release.notes.c_str());
}

void FirmwareUpdater::AnnouncePendingRelease()
{
    const String version = PendingRelease().version;
    SetStatus(Status::UpdateFound,
              autoInstall.load()
                  ? "Version " + version + " found - installing now"
                  : "Version " + version + " is ready to install");
}

bool FirmwareUpdater::RequestInstallNow()
{
    if (!updateAvailable.load())
        return false;

    const String version = PendingRelease().version;
    Serial.printf("[OTA] Install of %s confirmed from the configuration page\n", version.c_str());

    // Same ordering as the automatic path: announce first, because the render
    // loop acts on updatePending as soon as it is set.
    SetStatus(Status::UpdateFound, "Installing " + version);
    updatePending.store(true);
    return true;
}

FirmwareUpdater::Release FirmwareUpdater::PendingRelease() const
{
    Release copy;
    if (stateMutex != nullptr && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        copy = pendingRelease;
        xSemaphoreGive(stateMutex);
    }
    return copy;
}

bool FirmwareUpdater::Install(const Release& release, const ProgressCallback& onProgress)
{
    Serial.printf("[OTA] Installing %s from %s\n", release.version.c_str(), release.url.c_str());

    WiFiClientSecure client;
    client.setCACert(UpdateRootCAs::GitHubRoots);
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);  // seconds, unlike HTTPClient's

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);

    // The download is driven by hand rather than through HTTPUpdate because
    // this core's HTTPUpdate cannot be told the expected MD5 -- it only honours
    // an x-MD5 header, which release assets do not carry. Writing to Update
    // directly lets the digest from the manifest be the thing that decides
    // whether the image is allowed to boot.
    if (!http.begin(client, release.url)) {
        SetStatus(Status::Failed, "could not open firmware connection");
        return Abandon();
    }

    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        SetStatus(Status::Failed, "firmware download returned HTTP " + String(status));
        http.end();
        return Abandon();
    }

    const int reported = http.getSize();
    size_t total = reported > 0 ? static_cast<size_t>(reported) : release.size;
    if (total == 0) {
        SetStatus(Status::Failed, "firmware download has no length");
        http.end();
        return Abandon();
    }

    // A disagreement between the manifest and the asset means the release was
    // assembled wrongly. Refuse rather than trust one over the other.
    if (reported > 0 && release.size > 0 && static_cast<size_t>(reported) != release.size) {
        SetStatus(Status::Failed, "size mismatch: manifest says " + String(release.size) +
                     ", server sent " + String(reported));
        http.end();
        return Abandon();
    }

    // Picks the OTA slot that is not currently executing.
    if (!Update.begin(total, U_FLASH)) {
        SetStatus(Status::Failed, String("cannot start update: ") + Update.errorString());
        http.end();
        return Abandon();
    }

    // Checked by Update.end(). Until it passes, the boot slot is not switched.
    if (!Update.setMD5(release.md5.c_str())) {
        SetStatus(Status::Failed, "rejected md5 " + release.md5);
        Update.abort();
        http.end();
        return Abandon();
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[DOWNLOAD_CHUNK_BYTES];
    size_t written = 0;
    unsigned long lastProgressAt = millis();
    bool recordedFailure = false;

    // The web server keeps answering while this runs -- it lives on its own
    // task -- so anyone watching the configuration page sees the download move.
    int lastReportedPercent = -1;
    SetStatus(Status::Downloading, "Downloading " + release.version);

    while (written < total) {
        // Checked first thing every pass, so that any way of making no progress
        // -- no bytes available, or a socket that keeps returning zero from a
        // read -- ends the loop instead of spinning here forever.
        if (millis() - lastProgressAt > DOWNLOAD_STALL_TIMEOUT_MS) {
            SetStatus(Status::Failed, "download stalled after " + String(written) + " bytes");
            recordedFailure = true;
            break;
        }

        // Signed on purpose: available() reports TLS-layer errors as a negative
        // value, and folding that into a size_t would ask for a read of about
        // four billion bytes.
        const int available = stream->available();
        if (available <= 0) {
            // A closed connection with nothing buffered means the transfer
            // ended early; the length check below turns that into a failure.
            if (!client.connected())
                break;
            delay(1);
            continue;
        }

        const size_t wanted = std::min(static_cast<size_t>(available),
                                       std::min(DOWNLOAD_CHUNK_BYTES, total - written));
        const size_t read = stream->readBytes(buffer, wanted);
        if (read == 0) {
            delay(1);
            continue;
        }

        if (Update.write(buffer, read) != read) {
            SetStatus(Status::Failed, String("flash write failed: ") + Update.errorString());
            recordedFailure = true;
            break;
        }

        written += read;
        lastProgressAt = millis();
        if (onProgress)
            onProgress(written, total);

        // Every five percent rather than every chunk: this loop runs hundreds of
        // times a second and each update takes the state mutex.
        const int percent = static_cast<int>((written * 100) / total);
        if (percent >= lastReportedPercent + 5) {
            lastReportedPercent = percent;
            SetStatus(Status::Downloading,
                      "Downloading " + release.version + " - " + String(percent) + " percent");
        }
    }

    http.end();

    if (written != total) {
        if (!recordedFailure)
            SetStatus(Status::Failed, "download truncated at " + String(written) + " of " + String(total));
        Update.abort();
        return Abandon();
    }

    // Verifies the MD5 and, only then, points otadata at the new slot.
    if (!Update.end()) {
        SetStatus(Status::Failed, String("verification failed: ") + Update.errorString());
        return Abandon();
    }

    Serial.printf("[OTA] %s written and verified\n", release.version.c_str());
    SetStatus(Status::Installed, "Installed " + release.version + " - restarting");
    return true;
}

bool FirmwareUpdater::Abandon()
{
    // Nothing here undoes damage, because a failure cannot do any: the image is
    // written to the slot the radar is not running from, and otadata is only
    // moved by a successful Update.end(). Clearing the flag stops the render
    // loop from retrying the same broken download on the very next frame --
    // the hourly check will offer the release again.
    updatePending.store(false);
    updateAvailable.store(false);
    return false;
}

void FirmwareUpdater::SetStatus(Status newStatus, const String& message)
{
    if (newStatus == Status::Failed)
        Serial.printf("[OTA] %s\n", message.c_str());

    if (stateMutex != nullptr && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        status = newStatus;
        statusMessage = message;
        xSemaphoreGive(stateMutex);
    }
}

FirmwareUpdater::StatusSnapshot FirmwareUpdater::CurrentStatus() const
{
    StatusSnapshot snapshot;
    if (stateMutex != nullptr && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        snapshot.status = status;
        snapshot.message = statusMessage;
        xSemaphoreGive(stateMutex);
    }

    // Derived rather than stored, so it cannot disagree with the flags the
    // render loop and the checker task actually act on.
    snapshot.awaitingConfirmation = AwaitingConfirmation();
    return snapshot;
}

const char* FirmwareUpdater::StatusName(Status status)
{
    switch (status) {
        case Status::Checking:    return "checking";
        case Status::UpToDate:    return "up-to-date";
        case Status::UpdateFound: return "update-found";
        case Status::Downloading: return "downloading";
        case Status::Installed:   return "installed";
        case Status::Failed:      return "failed";
        case Status::Idle:
        default:                  return "idle";
    }
}

FirmwareUpdater::Version FirmwareUpdater::ParseVersion(const String& raw)
{
    Version version;

    String text = raw;
    text.trim();
    if (text.startsWith("v") || text.startsWith("V"))
        text.remove(0, 1);

    // Ignore any pre-release or build suffix -- "1.2.0-rc1" compares as 1.2.0.
    const int suffix = text.indexOf('-');
    if (suffix >= 0)
        text.remove(suffix);

    const int firstDot = text.indexOf('.');
    if (firstDot < 0)
        return version;
    const int secondDot = text.indexOf('.', firstDot + 1);
    if (secondDot < 0)
        return version;

    version.major = text.substring(0, firstDot).toInt();
    version.minor = text.substring(firstDot + 1, secondDot).toInt();
    version.patch = text.substring(secondDot + 1).toInt();
    version.valid = true;
    return version;
}

bool FirmwareUpdater::IsNewer(const Version& candidate, const Version& current)
{
    if (!candidate.valid)
        return false;

    // An unparseable local version means someone edited FIRMWARE_VERSION into
    // something odd. Refusing to update is the safe reading of that.
    if (!current.valid)
        return false;

    if (candidate.major != current.major)
        return candidate.major > current.major;
    if (candidate.minor != current.minor)
        return candidate.minor > current.minor;
    return candidate.patch > current.patch;
}
