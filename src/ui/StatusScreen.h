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
