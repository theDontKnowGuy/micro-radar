// Raised before esp_log.h so ESP_LOGW below is not compiled out.
//
// The Arduino core's sdkconfig sets CONFIG_LOG_MAXIMUM_LEVEL=1 (ERROR), which
// is a compile-time ceiling: ESP_LOGW would expand to nothing everywhere. Every
// warning in this firmware funnels through Warn() in this one file, so lifting
// the ceiling here lifts it for all of them without touching a build flag that
// would also change how the bundled libraries compile.
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include "Diagnostics.h"

#include <atomic>
#include <cstdarg>
#include <cstring>

#include <Preferences.h>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sdkconfig.h>

#include "ConfigurationWebServer.h"
#include "FirmwareVersion.h"

#ifdef CONFIG_ESP_INSIGHTS_ENABLED
#include <Insights.h>
#include <esp_rmaker_work_queue.h>
#endif

namespace {

// One tag for the whole firmware. The dashboard groups by tag, and splitting
// per module would scatter a single crash's story across several groups.
constexpr const char* TAG = "radar";

// Long enough for any message here; anything longer is truncated rather than
// allowed to cost a heap allocation on a path that runs during failures, which
// is exactly when the heap is the thing that has gone wrong.
constexpr size_t MessageLimit = 256;

bool started = false;
String nodeId;

// These two outlive Begin() on purpose, and the reason is not style.
//
// esp_insights_init() does not copy the auth key. It keeps the pointer:
//
//     g_default_insights_transport_https.userdata = (void *)config->auth_key;
//
// so a key handed over as a local String's c_str() is freed heap by the time
// the first report goes out. The transport then authenticates with whatever
// now occupies those bytes and the server answers 403 -- forever, and at the
// transport's retry rate rather than its reporting interval, which is how a
// mistake this small turns into the flood that TransportIsFlooding() below
// exists to stop. The same caution applies to the label: it is handed to the
// diagnostics store as a bare pointer.
String storedAuthKey;
String storedLabel;

// Bumped by the log hook whenever the transport complains, and read by Poll().
//
// A counter rather than a flag because occasional failures are normal -- a
// router reboots, a DNS lookup times out -- and shutting down over one would
// make the feature useless. What is not normal is a burst, and a burst is the
// dangerous shape: every retry is a TLS handshake, and this board does not have
// the contiguous heap to spare for a stream of them. The symptom is
// mbedtls_ssl_setup returning -0x7F00 (MBEDTLS_ERR_SSL_ALLOC_FAILED), at which
// point the aircraft fetch and the firmware updater are being starved by the
// thing that is only supposed to be watching them.
std::atomic<uint32_t> transportErrors{0};
std::atomic<unsigned long> transportWindowStartedAt{0};

// Raised by the log hook, lowered by Poll() when it shuts the agent down.
std::atomic<bool> transportFlooded{false};

// Armed by Begin() (or by a restart below) and consumed by the next Poll().
//
// A tidiness measure, and worth being honest about how little it buys: the
// thing that fixed the ten-second unsupervised storm is the Poll() pump around
// the address screen in setup(), not this. esp_insights_init() looks at
// esp_wifi_sta_get_ap_info() and queues a report of its own when it finds the
// station already associated, so the transport can start failing inside
// StartAgent() whatever this flag does. What deferring the explicit send is
// good for is narrower: it keeps the one upload this module controls from
// going out before the loop that watches for the consequences is running.
std::atomic<bool> firstSendPending{false};

// The diagnostics store is freed by StopAgent() and allocated by StartAgent(),
// and Event() writes into it from whichever task called it -- AircraftManager
// does so from "radar-network" on core 0 while Poll() runs on the loop task.
// Without this, the interleaving is: the network task passes the `started`
// check, the loop task frees the store inside esp_insights_disable(), the
// network task writes through the pointer that has just been NULLed. That is
// the same StoreProhibited this file already documents for rmaker_queue_ta.
//
// The race is not new, but the exposure is: teardown used to happen at most
// once per boot, and the one place it happened with other tasks running had
// already suspended them. The restart below makes it a recurring event on a
// live device, which turns a narrow window into one that will eventually be
// hit.
//
// It does not close the window completely, and nothing in this file can. Warn()
// and Error() reach the store through the IDF's own esp_log_write, which the
// Insights build wraps at link time -- that path is inside the library and
// takes no lock of ours. What is covered is Event(), which is the call that
// carries the strings worth having and the one this firmware makes most.
//
// The cost is that an Event() landing during a teardown waits for it, and a
// teardown is two to four seconds (see Poll() in the header). That falls on the
// network task, whose fetches already take seconds and which has nothing
// waiting on it in the meantime -- and the alternative to waiting is the
// use-after-free this exists to prevent.
SemaphoreHandle_t storeMutex = nullptr;

// Restart bookkeeping, touched only from Poll() and Begin() -- both on the
// Arduino loop task -- so plain variables are enough.
//
// The stop used to be final until someone power-cycled the radar. That was
// tolerable while it took a sustained storm to trigger; now that the detector
// actually fires, the common case it fires on is a router that has not got its
// upstream back yet at the moment the radar boots, and answering a thirty
// second outage by disabling reporting until the next reboot is not a good
// trade for a device on someone else's shelf.
//
// So the agent comes back, on a doubling delay: five minutes, ten, twenty, up
// to an hour. If the internet is genuinely gone, the cost settles at one short
// burst an hour rather than a permanent one; if it was a blip, reporting
// resumes on its own.
bool restartScheduled = false;
unsigned long agentStoppedAt = 0;
unsigned long agentStartedAt = 0;
constexpr unsigned long FirstRestartDelayMs = 5UL * 60UL * 1000UL;
constexpr unsigned long MaxRestartDelayMs = 60UL * 60UL * 1000UL;
unsigned long restartDelayMs = FirstRestartDelayMs;

// How long the agent has to run without storming before the doubling above is
// forgiven. Without it the delay only ever grows, so a unit that meets four
// unrelated blips over a couple of months is on the one-hour delay for the rest
// of its life -- which is the permanent shutdown this restart was written to
// avoid, arrived at slowly instead of at once.
//
// Half an hour is several of the agent's own 60-240 second reporting cycles, so
// a run that reaches it has demonstrably been uploading rather than merely
// sitting there between failures.
constexpr unsigned long StableRunMs = 30UL * 60UL * 1000UL;

// A burst is this many transport errors inside this window. Chosen against an
// observed failure: a rejected key produced errors roughly five times a second,
// so twenty in a minute is far above anything intermittent and is reached in
// seconds by a genuine storm.
constexpr uint32_t TransportErrorBurst = 20;
constexpr unsigned long TransportErrorWindowMs = 60000;

// Puts the burst count back to nothing, so that a restarted agent is judged on
// what it does next rather than on the errors that stopped the last one.
void ResetTransportWindow()
{
    transportErrors.store(0);
    transportWindowStartedAt.store(0);
    transportFlooded.store(false);
}

// Where the IDF's logs go once Begin() has run.
//
// Without this they go to stdout, which on this board is UART0 -- physical pins
// nobody has a wire on. Serial is USB CDC (ARDUINO_USB_CDC_ON_BOOT=1), so the
// mbedTLS and Wi-Fi driver errors that would explain a misbehaving unit have
// been going out a port that does not exist on the case.
//
// The `if (Serial)` matters more than it looks. HWCDC::write blocks until its
// transmit timeout when no host is draining the endpoint, and a deployed radar
// has no host at all -- so without the guard, every IDF log line would stall
// whichever task emitted it for the timeout. HWCDC's bool conversion is false
// when nothing is connected, which is the common case in the field and the
// whole reason this module exists.
// Counts a transport complaint, and reports whether they are now arriving fast
// enough to be a storm rather than weather.
//
// Reading the agent's own log output is not the way anyone would choose to
// learn this. The header declares INSIGHTS_EVENT_TRANSPORT_SEND_FAILED for
// exactly this purpose, but the HTTPS transport bundled with this core never
// posts it -- libesp_insights.a references esp_event_handler_register and
// nothing that emits, so the event is a promise the build does not keep. The
// log line is what the failure actually produces, and this module already sits
// on every log line for other reasons, so it costs nothing to notice.
//
// If Espressif renames the tag in a later core, this stops tripping and the
// behaviour falls back to what it was before -- noisy, not broken.
bool TransportIsFlooding(const char* line)
{
    if (std::strstr(line, "tport_https") == nullptr)
        return false;

    const unsigned long now = millis();
    const unsigned long windowStart = transportWindowStartedAt.load();

    if (windowStart == 0 || now - windowStart > TransportErrorWindowMs) {
        transportWindowStartedAt.store(now);
        transportErrors.store(1);
        return false;
    }

    return transportErrors.fetch_add(1) + 1 >= TransportErrorBurst;
}

int WriteLogToSerial(const char* format, va_list args)
{
    char buffer[MessageLimit];
    const int length = vsnprintf(buffer, sizeof(buffer), format, args);
    if (length <= 0)
        return length;

    // Noticed here rather than acted on here: this runs on the agent's own task,
    // inside its logging call, and tearing the agent down from there would be
    // pulling the floor out from under the caller. Poll() does it from the
    // render loop instead -- the same arrangement RestartRequested() and
    // UpdatePending() already use for the same reason.
    if (TransportIsFlooding(buffer))
        transportFlooded.store(true);

    if (Serial) {
        const size_t written = (static_cast<size_t>(length) < sizeof(buffer))
            ? static_cast<size_t>(length)
            : sizeof(buffer) - 1;
        Serial.write(reinterpret_cast<const uint8_t*>(buffer), written);
    }

    return length;
}

// Turns a reset reason into something readable on a dashboard. The numeric
// value is what esp_reset_reason() gives and what the current boot log prints;
// six weeks later, looking at a crash from someone else's house, "TASK_WDT" is
// worth considerably more than "6".
// Counts boots, in its own NVS namespace so it cannot collide with a settings
// key. One small write per boot, which NVS wear levelling will outlive by
// several lifetimes of the device.
//
// It earns its place twice. It makes each boot event distinct -- "v1.7.2,
// reset: software" is identical every time, which is indistinguishable from one
// event on a dashboard that groups by text, and that ambiguity is currently in
// the way of knowing whether reports are arriving at all. And a reboot count is
// the number you actually want from a radar in someone else's house: a unit on
// boot 400 is in a loop, whatever its last message said.
uint32_t RecordBoot()
{
    Preferences diagPrefs;
    if (!diagPrefs.begin("diag", false))
        return 0;

    const uint32_t count = diagPrefs.getUInt("boots", 0) + 1;
    diagPrefs.putUInt("boots", count);
    diagPrefs.end();
    return count;
}

#ifdef CONFIG_ESP_INSIGHTS_ENABLED
// Shuts the agent down without pulling the rug out from under its own task.
//
// esp_insights_deinit() tears down in the wrong order for an agent that is
// still working. It calls, in this sequence:
//
//     esp_insights_transport_disconnect();
//     esp_insights_disable();          // frees the diagnostics store, the RTC
//                                      // log store, the metric and variable
//                                      // tables, and NULLs them
//     esp_insights_transport_unregister();
//     esp_rmaker_work_queue_deinit();  // only now stops the task using them
//
// and everything the agent uploads runs on the rmaker work queue task, because
// esp_insights_send_data() does nothing but queue a work item for it. So a
// send() immediately followed by end() -- which is what the two callers below
// used to do -- hands the task an upload and then frees the buffers it is
// reading from mid-post. The task writes through a pointer that has just been
// zeroed and the device dies with StoreProhibited at address 0 in
// "rmaker_queue_ta". Nothing in the app's own backtrace, because it is not the
// app's stack. This is what a radar was crashing on partway through an OTA.
//
// Stopping the queue first closes the window. esp_rmaker_work_queue_deinit()
// raises the stop flag and blocks until the task has finished whatever it is
// posting and deleted itself -- its receive has a two second timeout, so this
// costs at most that plus the tail of one upload -- and only then deletes the
// queue. By the time Insights.end() runs there is no task left to trip over the
// teardown, and the esp_rmaker_work_queue_deinit() inside it finds the queue
// already gone and returns.
//
// Held across the whole teardown rather than around Insights.end() alone: the
// point of the lock is that no other task is inside esp_diag_log_event() while
// the store it writes to is being freed, and `started` has to be lowered under
// the same lock or a waiter would acquire it and walk straight into the check
// it already passed. See storeMutex.
void StopAgent()
{
    if (storeMutex != nullptr)
        xSemaphoreTake(storeMutex, portMAX_DELAY);

    esp_rmaker_work_queue_deinit();
    Insights.end();
    started = false;
    firstSendPending.store(false);

    if (storeMutex != nullptr)
        xSemaphoreGive(storeMutex);
}

// Declares one of this firmware's dashboard variables, and says so when it
// cannot.
//
// The return value is worth a line because the failure is silent and the
// restart path is what could produce it. The table holds
// CONFIG_DIAG_VARIABLES_MAX_COUNT entries -- 20, shared with the ones the
// network stack registers for itself -- and it is freed and rebuilt on every
// restart. esp_insights_disable() does call esp_diag_variables_deinit(), so a
// cycle that leaked entries would be a library bug rather than one of ours; the
// point of noticing is that if it ever happens, the symptom on the dashboard is
// a device that quietly goes back to being an unlabelled MAC address, which
// looks like a different fault entirely.
void RegisterVariable(const char* key, const char* label)
{
    if (!Insights.variables.addString("radar", key, label, "device"))
        Serial.printf("Diagnostics: could not register the '%s' variable\n", key);
}

// Brings the agent up, and is called for the first start and for every restart
// after a storm. Everything here has to be redone on a restart rather than only
// on the first start: Insights.end() frees the variable table along with the
// rest of the diagnostics store, so a resumed agent that did not re-declare
// these would report as an unlabelled node on an unknown firmware version.
//
// Deliberately does not send. The caller arms firstSendPending instead, so the
// upload happens on the next Poll() with the burst detector already watching.
bool StartAgent()
{
    // Large buffers into PSRAM. The 360x360 backbuffer already showed that this
    // board's contiguous SRAM is the scarce thing, and the agent's buffers have
    // no reason to compete for it.
    const bool useExternalRam = ESP.getPsramSize() > 0;

    // Node id left null so the agent uses the Wi-Fi MAC, which is unique
    // without anyone having to keep a list. The human label is attached below
    // as a variable instead -- two friends both typing "living room" would
    // otherwise merge into one device on the dashboard.
    if (!Insights.begin(storedAuthKey.c_str(), nullptr, 0xFFFFFFFF, useExternalRam))
        return false;

    started = true;
    agentStartedAt = millis();
    nodeId = Insights.nodeID();

    // Shown beside each device on the dashboard. Without it, twenty radars are
    // twenty MAC addresses and no way to tell whose is crashing.
    if (!storedLabel.isEmpty()) {
        RegisterVariable("label", "Label");
        Insights.variables.setString("label", storedLabel.c_str());
    }

    // So a crash can be read against the build it came from. The dashboard
    // groups by version, which is what turns "it crashes sometimes" into "it
    // started crashing in 1.7.0".
    RegisterVariable("fw", "Firmware");
    Insights.variables.setString("fw", FIRMWARE_VERSION);

    // A restarted agent starts its burst count from nothing. Without this the
    // errors that stopped it are still in the window, and the first failure
    // after the restart would trip the detector again immediately.
    ResetTransportWindow();
    return true;
}
#endif

const char* ResetReasonName(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_EXT:      return "external-pin";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "interrupt-watchdog";
        case ESP_RST_TASK_WDT: return "task-watchdog";
        case ESP_RST_WDT:      return "other-watchdog";
        case ESP_RST_DEEPSLEEP:return "deep-sleep-wake";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

} // namespace

namespace Diagnostics {

void Begin(ConfigurationWebServer& config)
{
    // Unconditional, and ahead of the enabled check: bringing the IDF's own
    // logs to the USB console is worth having on a board on a desk with no
    // reporting configured at all.
    esp_log_set_vprintf(WriteLogToSerial);
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);

    const esp_reset_reason_t resetReason = esp_reset_reason();

#ifdef CONFIG_ESP_INSIGHTS_ENABLED
    storedAuthKey = config.GetStoredString("insights-key");
    storedAuthKey.trim();

    if (storedAuthKey.isEmpty()) {
        Serial.println("Diagnostics: no auth key stored - remote reporting is off");
        return;
    }

    // Before anything can write to the store, which is what makes it safe for
    // Event() to test the handle without a lock of its own: this runs in
    // setup(), ahead of AircraftManager::Initialise() and so ahead of the only
    // other task that calls Event(). A failure here is not fatal -- both users
    // fall back to running unlocked, which is the behaviour this had before --
    // but it is worth saying out loud rather than leaving as a silent
    // difference in how the device behaves under teardown.
    storeMutex = xSemaphoreCreateMutex();
    if (storeMutex == nullptr)
        Serial.println("Diagnostics: no mutex for the event store - teardown is unguarded");

    // Read before the agent starts because StartAgent() declares it as a
    // variable, and is kept in a String that outlives this call for the reason
    // at the top of this file.
    storedLabel = config.GetStoredString("insights-label");
    storedLabel.trim();

    if (!StartAgent()) {
        // Deliberately not an Error(): the reporting channel is what just
        // failed, so sending this through it would go nowhere.
        Serial.println("Diagnostics: agent failed to start - check the auth key");
        return;
    }

    Serial.printf("Diagnostics: reporting as %s\n", nodeId.c_str());

    // The first thing every boot reports, and the one that makes a crash loop
    // legible: a device whose events are a column of "panic" is telling a very
    // different story from one that reads "power-on".
    Event("boot", "#%u v%s, reset: %s",
          RecordBoot(), FIRMWARE_VERSION, ResetReasonName(resetReason));

    // Armed rather than sent, and Poll() does it -- see firstSendPending. It
    // still goes out within milliseconds of the first pass of whichever loop
    // follows setup(), which keeps what this was for: the agent's own schedule
    // is 60 to 240 seconds away, and waiting four minutes to find out whether a
    // key works is the difference between a setting you can check and one you
    // have to believe in.
    firstSendPending.store(true);
#else
    (void)config;
    Serial.println("Diagnostics: ESP Insights is not compiled into this core");
#endif
}

bool Enabled()
{
    return started;
}

void Poll()
{
#ifdef CONFIG_ESP_INSIGHTS_ENABLED
    if (started) {
        // Before the flood check rather than after it, so that a send which
        // turns into a storm is stopped by the very next pass rather than
        // sitting unqueued behind one.
        //
        // exchange() so this happens once even if two callers race -- the
        // OpenSky hold loop and loop() are different loops, not concurrent
        // ones, but the flag is also written from Begin() and a plain
        // load-then-store would be the kind of thing that only misbehaves in
        // the field.
        if (firstSendPending.exchange(false))
            Insights.send();

        if (!transportFlooded.load()) {
            // A run this long has been through several of the agent's own
            // 60-240 second reporting cycles without complaint, so whatever
            // stopped it last time is over and the escalated delay it earned
            // should not be charged against the next unrelated blip. See
            // StableRunMs.
            if (restartDelayMs != FirstRestartDelayMs &&
                millis() - agentStartedAt > StableRunMs)
                restartDelayMs = FirstRestartDelayMs;

            return;
        }

        transportFlooded.store(false);

        // Straight to Serial. Error() would route this through the log hook that
        // just tripped, and a shutdown notice is not worth another lap through the
        // machinery that is being shut down.
        Serial.println("Diagnostics: transport failing repeatedly - stopping the agent");
        Serial.printf("Diagnostics: retrying in %lu minutes - if this repeats, check the auth key"
                      " on the configuration page\n",
                      restartDelayMs / 60000UL);

        StopAgent();

        restartScheduled = true;
        agentStoppedAt = millis();
        return;
    }

    // Not running. Either it never started -- no key, or a key the agent
    // refused, neither of which a retry would change -- or a storm stopped it
    // and it is waiting out the delay.
    if (!restartScheduled)
        return;

    if (millis() - agentStoppedAt < restartDelayMs)
        return;

    // Ahead of the backoff below, and that order is the whole point: a radar
    // with no network has not tried anything, so it has nothing to back off
    // from. Charging it anyway walked the delay from five minutes to the hour
    // cap in about two hours of downtime, and left reporting dark for up to
    // another hour after the router came back -- an outage extending its own
    // recovery, which is the opposite of what a backoff is for.
    //
    // The storm case -- associated, but nothing reachable behind the router --
    // looks connected here, which is why the retry exists rather than this
    // check standing in for it. Cheap enough to run per pass: WiFi.status() is
    // a read of an event group bit.
    if (WiFi.status() != WL_CONNECTED)
        return;

    // Charged now that a restart is genuinely being attempted. Doubled here
    // rather than at the stop so the delay charged is the one that was
    // announced.
    const unsigned long nextDelay = restartDelayMs * 2;
    restartDelayMs = nextDelay > MaxRestartDelayMs ? MaxRestartDelayMs : nextDelay;
    agentStoppedAt = millis();

    Serial.println("Diagnostics: restarting the reporting agent");

    if (!StartAgent()) {
        Serial.println("Diagnostics: agent would not restart - waiting again");
        return;
    }

    // Distinct from "boot" on purpose: this is the event that says a gap in a
    // device's history was an outage rather than a device that was switched
    // off, which is the question anyone reading a sparse timeline asks first.
    Event("insights", "reporting resumed after transport failures");
    firstSendPending.store(true);
#endif
}

String NodeId()
{
    return nodeId;
}

void PauseForUpdate()
{
#ifdef CONFIG_ESP_INSIGHTS_ENABLED
    if (!started)
        return;

    // A last word before going quiet. Sent synchronously-ish rather than left
    // to the next scheduled post, because the next scheduled post will not
    // happen -- this is followed by a flash rewrite and a reboot.
    //
    // StopAgent() rather than Insights.end() directly, and the reason is the
    // send() on the line above: it queues the upload rather than performing it,
    // so the teardown has to wait for the queue. See StopAgent(). If the task
    // stops before it gets to this event, the event is not lost -- it stays in
    // the RTC store, which survives the reboot, and goes out with the first
    // report from the new firmware.
    Event("update", "installing, going quiet");
    Insights.send();
    StopAgent();

    // Cancelled belt-and-braces rather than because anything can currently act
    // on it: every caller follows this with UpdateScreen::RunFirmwareUpdate(),
    // which blocks until it reboots on both the success and the failure path
    // and never calls Poll(). So there is no pass of any loop between here and
    // the reset in which a restart could fire. The line costs nothing and means
    // a future caller that does return here cannot bring the agent back up on
    // top of a flash write.
    restartScheduled = false;
#endif
}

void Event(const char* tag, const char* format, ...)
{
    char message[MessageLimit];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Always on the console, whether or not anything is being reported.
    Serial.printf("[%s] %s\n", tag, message);

#ifdef CONFIG_ESP_INSIGHTS_ENABLED
    // Under the lock, and the `started` test has to be inside it rather than in
    // front of it. AircraftManager calls this from the "radar-network" task
    // while Poll() can be tearing the agent down on the loop task, and a check
    // made outside the lock is a check that was true a moment before the store
    // it approved was freed. See storeMutex.
    if (storeMutex != nullptr)
        xSemaphoreTake(storeMutex, portMAX_DELAY);

    if (!started) {
        if (storeMutex != nullptr)
            xSemaphoreGive(storeMutex);
        return;
    }

    // "%s" rather than passing the caller's format through: the text has
    // already been expanded, and handing an arbitrary string to a printf-style
    // API as its format is how a stray '%' in a callsign or an SSID turns a log
    // call into a read of whatever is next on the stack.
    //
    // The return is checked because the interesting failure here is silent. The
    // agent buffers into RTC memory -- 6KB of it, and this core is built with
    // CONFIG_RTC_STORE_OVERWRITE_NON_CRITICAL_DATA off, so a full buffer drops
    // new data rather than discarding old. The buffer is only emptied by a
    // successful upload and it survives a software reset, so a spell of failing
    // uploads can leave a device that starts cleanly, reports no errors, and
    // quietly records nothing. Without this line that state is indistinguishable
    // from working.
    const esp_err_t stored = esp_diag_log_event(tag, "%s", message);

    if (storeMutex != nullptr)
        xSemaphoreGive(storeMutex);

    if (stored != ESP_OK)
        Serial.printf("Diagnostics: event dropped (0x%x) - buffer full? power-cycle to clear\n",
                      stored);
#endif
}

void Warn(const char* format, ...)
{
    char message[MessageLimit];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    ESP_LOGW(TAG, "%s", message);
}

void Error(const char* format, ...)
{
    char message[MessageLimit];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    ESP_LOGE(TAG, "%s", message);
}

} // namespace Diagnostics
