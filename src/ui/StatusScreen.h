#pragma once

#include <Arduino.h>

#include "LGFX.h"

// Centres up to five lines of status text on an otherwise blank panel. Every
// screen the radar shows before it starts sweeping looks like this. The text is
// set in the largest size that still keeps the longest line clear of the bezel
// -- these screens are read from across a room, and the panel is round, so the
// usable width is a little short of SCREEN_SIZE.
void ShowStatusScreen(LGFX& tft,
                      const String& first,
                      const String& second = "",
                      const String& third = "",
                      const String& fourth = "",
                      const String& fifth = "");

// The address screen: a heading, the radar's address as a QR code under it, and
// the same address in text under that.
//
// The order is what the screen is for. The heading says what the thing below it
// is, the code is the way anyone with a phone in their hand should take, and the
// text is for everyone else -- and is what the code resolves to, which is worth
// being able to see before scanning anything.
//
// The arguments are in that order for a second reason: this screen is set in two
// sizes, not one. `heading` and `address` get the largest face they both fit in,
// and `fallback` and `footnote` get the largest face *they* fit in, which on a
// round panel is usually a step smaller -- a numeric address with a scheme in
// front of it is half again as wide as the name it stands in for.
//
// That split is the whole point. Sized as one block, a single over-long line
// takes every other line down with it, and the address nobody can read is the
// one they were meant to read first.
void ShowQrAddressScreen(LGFX& tft,
                         const String& heading,
                         const String& address,
                         const String& fallback = "",
                         const String& footnote = "");
