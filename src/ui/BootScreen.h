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

// Holds the logo on screen for the rest of BootScreen::HoldMs, counted from
// `startedAt` -- which is taken before the Wi-Fi join starts, so the join and
// the logo share the same stretch of time instead of running one after the
// other. Returns once the hold is up; whether the join finished in that window
// is the caller's business.
void Hold(LGFX& tft, unsigned long startedAt);

}
