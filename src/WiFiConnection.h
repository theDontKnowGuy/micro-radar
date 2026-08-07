#pragma once

#include "LGFX.h"

class ConfigurationWebServer;
class FirmwareUpdater;

// Getting the radar onto a network at boot, and putting one up itself when it
// cannot. The credentials found by BeginJoin are held in this module between
// the two halves of the join, so setup() does not have to carry them.
namespace WiFiConnection {

// The whole budget for getting onto a network at boot, counted from the instant
// the boot logo goes up -- the join runs underneath the logo, so this is spent
// looking at the boot screen rather than added on after it. Long enough for a
// router still coming back up after a power cut, which the driver's own retries
// (see BeginJoin) need a few passes to ride out; short enough that a radar with
// a mistyped password reaches the setup hotspot while its owner is still
// standing there.
constexpr unsigned long JoinTimeoutMs = 20000;

// Reads the stored (or pre-baked) credentials and kicks the join off without
// waiting for it, so the boot logo and the association run over the same
// stretch of time rather than one after the other. Does nothing if there is
// nothing to join.
void BeginJoin(ConfigurationWebServer& configServer);

// Whether the join is still worth waiting on: credentials were found and the
// driver has not associated yet. False with nothing stored, since no amount of
// waiting turns that into a network. Cheap enough to poll every few
// milliseconds, which is what the boot screen does with it while it holds the
// logo up.
[[nodiscard]] bool JoinPending();

// Ends the join one way or the other and reports whether it landed, dropping
// the credentials either way -- nothing past here needs them, and the driver
// has its own copy of the pair it is actually using. False means the radar has
// no network: nothing stored, or the stored network would not have us.
[[nodiscard]] bool FinishJoin();

// No network to join: become one. The same configuration page is served over
// the radar's own access point, so the network, the radar centre, the OpenSky
// credentials and everything else are all set in a single submission -- which
// is what the reboot at the end of this is for. Does not return.
[[noreturn]] void RunSetupPortal(LGFX& tft, ConfigurationWebServer& configServer, FirmwareUpdater& updater);

}
