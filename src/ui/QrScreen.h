#pragma once

#include "LGFX.h"

// The QR code for the project's GitHub page, shown once at boot, straight after
// the logo. Nothing on the radar face has room for a URL, and a forty-five
// character address read off a two-inch panel and typed into a phone is not
// something anyone does twice -- so it goes out in the one form a phone can
// take in at a glance, in the stretch of the boot that already belongs to the
// branding rather than to anything the owner is waiting for.
namespace QrScreen {

// How long the code stays up. Long enough to notice it, raise a phone and let
// it lock on, and short enough that a radar rebooting in front of someone who
// has no interest in the repository is not held up by it.
constexpr unsigned long HoldMs = 5000;

// Paints the code, holds it for HoldMs, and leaves the panel black. Blocking,
// like the boot screen it follows: this is setup(), and nothing else is running
// yet.
void Show(LGFX& tft);

}
