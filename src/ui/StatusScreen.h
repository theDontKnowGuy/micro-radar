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

// The same screen with the project's QR code across the top of it, and the
// lines fitted into what is left underneath.
//
// For the address screen, which is where someone is already looking at the
// panel with a phone in their hand. The type comes out smaller than the plain
// version's, deliberately: the badge is what the top of the panel is for now,
// and an address anyone can reach by scanning does not need to be legible from
// the other side of the room as well.
void ShowStatusScreenWithQr(LGFX& tft,
                            const String& first,
                            const String& second = "",
                            const String& third = "",
                            const String& fourth = "",
                            const String& fifth = "");
