#pragma once

#include <Arduino.h>

#include "LGFX.h"

// Centres up to five lines of status text on an otherwise blank panel. Every
// screen the radar shows before it starts sweeping looks like this. The text is
// scaled up to the largest whole multiple that still keeps the longest line
// clear of the bezel -- these screens are read from across a room, and the
// panel is round, so the usable width is a little short of SCREEN_SIZE.
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
// lines underneath are for everyone else -- and are what the code resolves to,
// which is worth being able to see before scanning anything.
//
// The type comes out smaller than the plain version's. There is less room, and
// less need: an address that can be scanned does not also have to be legible
// from the other side of the room.
void ShowQrAddressScreen(LGFX& tft,
                         const String& heading,
                         const String& first,
                         const String& second = "",
                         const String& third = "");
