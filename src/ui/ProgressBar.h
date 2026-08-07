#pragma once

#include "LGFX.h"

// The loading bar the radar shows whenever it is busy with something that takes
// long enough to look like a hang: an outlined trough that fills left to right.
// Both users draw it straight to the panel, so both have to own the display for
// the duration -- see the note on UpdateScreen::RunFirmwareUpdate.
namespace ProgressBar {

constexpr int Height = 8;
constexpr int Width = SCREEN_SIZE / 2;
constexpr int X = (SCREEN_SIZE - Width) / 2;

// Half the height, so the caps are semicircles and the bar reads as a pill
// rather than as a rectangle with the corners knocked off.
constexpr int Radius = Height / 2;

void DrawOutline(LGFX& tft, int y, uint32_t color);

// The fill is drawn on the trough's own rectangle rather than inset inside it,
// and so grows over the outline instead of within it. Inset by even a single
// pixel, the cap loses two rows off an eight-row bar and the rounding that
// survives is a one-pixel chamfer -- the left end reads as square next to the
// trough's. Sharing the geometry makes the filled end exactly the shape of the
// trough end it is covering.
void DrawFill(LGFX& tft, int y, int percent, uint32_t color);

}
