#pragma once

#include "LGFX.h"

// The logo the radar shows while it boots, with a progress bar under it.
namespace BootScreen {

// How long the logo stays up. The bar is paced off this rather than off the
// Wi-Fi join running underneath it: the join takes anywhere from a second to
// the full timeout, and a bar that stalls at 40% reads as a hang. This is the
// floor on the logo, not the ceiling on the join -- setup() carries on waiting
// for the network afterwards if it needs to.
constexpr unsigned long HoldMs = 7000;

// Paints the logo and the empty trough, and returns. Nothing here blocks, so
// the caller can get the Wi-Fi join started before holding the screen.
void Draw(LGFX& tft);

// Holds the logo on screen for at least BootScreen::HoldMs and at most
// `maxHoldMs`, both counted from `startedAt` -- which is taken before the Wi-Fi
// join starts, so the join and the logo share the same stretch of time instead
// of running one after the other.
//
// Once the bar is full the logo stays up, unchanged, for as long as
// `stillWaiting` keeps saying the caller has something outstanding, up to that
// ceiling; it returns the moment the wait is over. Seven seconds is what the
// bar is paced for, not what the caller is owed -- and a bar that filled a
// second time, or a screen that changed while the join was still going, would
// read as a hang or a restart.
void Hold(LGFX& tft, unsigned long startedAt, bool (*stillWaiting)(), unsigned long maxHoldMs);

}
