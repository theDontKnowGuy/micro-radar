#pragma once

#include <LovyanGFX.hpp>

// The mechanics of turning a packed QR module grid into pixels -- the part
// ConfigQr and WifiSetupQr share. Each of those owns one generated grid (see
// scripts/generate_qr_code.py) and a static_assert tying it back to the string
// it is supposed to encode; this just draws whichever grid it is handed.
//
// Drawn on a LovyanGFX rather than the panel type for the same reason the
// progress bar is: with a rotation trim set it lands in a buffer that
// PanelTrim turns on the way out.
namespace QrBadge {

// A generated code's module grid, in the form scripts/generate_qr_code.py
// emits: one bit per module, MSB first, PROGMEM, dark set.
struct Code {
  const uint8_t* modules;
  int size;        // modules per side, without the quiet zone
  int quietZone;   // clear modules the code needs around it before a decoder will find it
  int stride;      // bytes per row of `modules`; the last one may be only partly used
};

// The side of the largest badge that fits within `maxSize` pixels, quiet zone
// included. Whole pixels per module always, so the answer steps rather than
// scales -- it is usually a good deal smaller than what was asked for, and that
// is the point: a module drawn 2.4 pixels wide is drawn 2 wide most of the time
// and 3 the rest, and a decoder reading a grid whose spacing wanders is a
// decoder that gives up.
//
// Zero if not even one pixel per module fits, in which case there is nothing
// worth drawing and the caller should lay itself out without a badge.
int SizeWithin(const Code& code, int maxSize);

// Draws a badge of exactly `size` pixels square -- the number SizeWithin() gave
// back -- with its top-left corner at (x, y). Any other size is rounded down to
// one this can actually draw.
void Draw(LovyanGFX& canvas, const Code& code, int x, int y, int size);

}
