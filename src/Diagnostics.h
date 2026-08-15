#pragma once

#include <Arduino.h>

class ConfigurationWebServer;

// Remote diagnostics for radars that are not on the end of a USB cable.
//
// scripts/pull-coredump.sh is the right tool for a board on a desk and useless
// for one on a friend's shelf: there is no port to read, no console anyone is
// watching, and the unit sits behind a router that nothing can reach in
// through. This module is the outbound half -- the radar reporting on itself.
//
// It is a thin layer over ESP Insights, which is Espressif's own remote
// diagnostics service and, usefully, is already compiled into the Arduino core
// we build against: libesp_insights.a, libesp_diagnostics.a and librtc_store.a
// all ship in the esp32s3 SDK, so this costs a header and no new dependency.
// The agent posts over HTTPS every 60-240 seconds -- there is no persistent
// socket, which matters on a board with a history of "SSL - Memory allocation
// failed" -- and on a panic it reports the exception cause, the registers and
// the backtrace from the coredump the panic handler already writes to flash.
//
// What it gives you, per device, on a web dashboard:
//   * crashes, with the line of code that died
//   * whatever is logged through Warn()/Error()/Event() below
//   * free heap, Wi-Fi RSSI, IP, SSID, uptime, over time
//
// What it does NOT give you is the full coredump with every task's stack. The
// agent sends a summary to keep the upload small. For the crashes a backtrace
// cannot explain you still need the dump itself, and that is the larger design
// in docs/remote-logging-design.md, not implemented here.
//
// OFF BY DEFAULT, and deliberately so. It stays off until someone enters an
// auth key on the configuration page, for two reasons:
//
//   1. Firmware images are published on public GitHub releases. A key compiled
//      into the binary is a public key -- `strings` would find it. Keeping it
//      in NVS keeps it out of the image, and lets one unit's key be revoked
//      without a release.
//   2. These radars live in other people's houses. The logs carry their SSID,
//      their IP and their uptime. Collecting that silently is not ours to do,
//      so it takes a deliberate action by whoever set the radar up.
namespace Diagnostics {

// Starts the agent if an auth key is stored, and does nothing if not. Call
// after Wi-Fi is up -- the first report goes out within a minute or two.
//
// Also installs the log redirect described in Warn() below, which happens
// whether or not reporting is enabled, because it improves the serial console
// on its own.
void Begin(ConfigurationWebServer& config);

// True when an auth key was stored and the agent started. The configuration
// page reports this so a mistyped key does not look like a working one.
[[nodiscard]] bool Enabled();

// The device's identity on the dashboard -- the Wi-Fi MAC, unless a label was
// set. Empty when reporting is off.
[[nodiscard]] String NodeId();

// Call once per pass of the render loop, and from any loop in setup() that runs
// for long enough to matter. Cheap -- two atomic loads in the common case.
//
// It does three things, all of which have to happen somewhere that is running
// often rather than on the agent's own task:
//
//   * sends the first report, which Begin() arms rather than performs;
//   * shuts the agent down if its transport has started failing in bursts;
//   * brings it back, on a doubling delay, once one has.
//
// The shutdown is not a tidiness measure. Every retry the transport makes is a
// fresh TLS handshake, and a router with no working internet behind it makes
// them about a hundred times a second; this board does not have the contiguous
// heap for that alongside the aircraft fetch and the firmware updater, and what
// it looks like when it runs out is mbedtls_ssl_setup failing with -0x7F00 in
// whichever component asked second. Reporting is the least important thing on
// the radar, so it is the thing that gives way.
//
// Which is why the call sites matter more than they look. Anything in setup()
// that waits for ten seconds without calling this gives the agent ten seconds
// of unsupervised storm, and the boot is exactly when both the aircraft task
// and the update checker want a TLS session of their own.
//
// NOT always cheap, and the exception is worth knowing about before adding a
// call site. A pass that stops the agent blocks its caller for roughly two
// seconds and up to four: esp_rmaker_work_queue_deinit() waits on the worker in
// vTaskDelay(2000) steps at CONFIG_FREERTOS_HZ=1000, against a worker whose own
// receive timeout is the same two seconds. On the render loop that is a frozen
// sweep for the duration.
//
// It buys a bounded cost in place of an unbounded one -- the alternative is the
// storm running until something else fails -- but the arithmetic changed when
// the restart went in. It used to be paid once per boot; a device whose key is
// genuinely rejected now pays it at t+5, +15, +35 and +75 minutes and then
// hourly, each time preceded by a short deliberate burst of failed handshakes
// in whatever heap AircraftManager is using. The burst is about a fifth of a
// second at the rate these arrive, so the trade holds, but a caller adding a
// third loop that pumps this should know it is not free.
void Poll();

// Stops the agent before a firmware install. The download wants the only TLS
// session this board has heap for, and UpdateScreen parks everything else for
// the same reason; a diagnostics post landing in the middle of a flash write is
// a risk with no upside. There is no matching Resume() because an install ends
// in a reboot, which starts the agent again from scratch.
void PauseForUpdate();

// A notable thing happened. Events always reach the dashboard, regardless of
// log level, so this is the one to reach for when you want to be sure you will
// see it: booted, joined a network, installed a release.
void Event(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));

// Something is wrong, or wrong enough to explain a complaint later.
//
// These go through the IDF's own logging rather than Serial, because that is
// the path the Insights agent taps -- the linker wraps esp_log_write, which is
// why a plain Serial.printf cannot be collected no matter how it is worded.
//
// They still appear on the serial console: Begin() points esp_log_set_vprintf()
// at Serial, which also brings across the IDF's own messages -- Wi-Fi
// association failures, mbedTLS errors, heap complaints. Those are exactly the
// messages that explain a sulking remote unit, and none of them have ever been
// visible on this board's USB console before.
void Warn(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Error(const char* format, ...) __attribute__((format(printf, 1, 2)));

} // namespace Diagnostics
