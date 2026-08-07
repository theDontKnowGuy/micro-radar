#pragma once

#include "LGFX.h"

class ConfigurationWebServer;
class FirmwareUpdater;

// Getting the radar onto a network at boot, and putting one up itself when it
// cannot. The credentials found by BeginJoin are held in this module between
// the two halves of the join, so setup() does not have to carry them.
namespace WiFiConnection {

// Reads the stored (or pre-baked) credentials and kicks the join off without
// waiting for it, so the boot logo and the association run over the same
// stretch of time rather than one after the other. Does nothing if there is
// nothing to join.
void BeginJoin(ConfigurationWebServer& configServer);

// Waits out the join BeginJoin started, counting the first attempt's timeout
// from `bootStartedAt` -- the same instant the logo was paced from. Draws its
// own status screens, since by the time it is called the logo is down.
// Returns false when the radar has no network: nothing stored, or the stored
// network would not have us.
[[nodiscard]] bool AwaitJoin(LGFX& tft, unsigned long bootStartedAt);

// No network to join: become one. The same configuration page is served over
// the radar's own access point, so the network, the radar centre, the OpenSky
// credentials and everything else are all set in a single submission -- which
// is what the reboot at the end of this is for. Does not return.
[[noreturn]] void RunSetupPortal(LGFX& tft, ConfigurationWebServer& configServer, FirmwareUpdater& updater);

}
