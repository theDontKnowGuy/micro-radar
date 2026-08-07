#pragma once

#include "FirmwareUpdater.h"
#include "LGFX.h"

namespace UpdateScreen {

// Takes the panel over for the duration of a firmware download. Call from the
// render loop, so no sprite push is in flight and drawing straight to the
// display is safe here even though it is not during normal operation.
//
// Does not return in practice: every path out of an install -- finished or
// failed -- ends in a reboot.
void RunFirmwareUpdate(LGFX& tft, FirmwareUpdater& updater);

}
