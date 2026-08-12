#pragma once

#include <LovyanGFX.hpp>

// The radar's own configuration address as a QR code, for the screen that asks
// someone to go and open it.
//
// The address is short and the panel is small, and neither of those is the
// reason this exists: an address read off a two-inch panel has to be typed into
// a phone by hand, and a name with a dot in it typed into a phone by hand is a
// search query about half the time. A code is the same address in the one form
// a phone cannot mistype.
//
// Drawn on a LovyanGFX rather than the panel type for the same reason the
// progress bar is: with a rotation trim set it lands in a buffer that PanelTrim
// turns on the way out.
namespace ConfigQr {

// The side of the largest badge that fits within `maxSize` pixels, quiet zone
// included. Whole pixels per module always, so the answer steps rather than
// scales -- it is usually a good deal smaller than what was asked for, and that
// is the point: a module drawn 2.4 pixels wide is drawn 2 wide most of the time
// and 3 the rest, and a decoder reading a grid whose spacing wanders is a
// decoder that gives up.
//
// Zero if not even one pixel per module fits, in which case there is nothing
// worth drawing and the caller should lay itself out without a badge.
int SizeWithin(int maxSize);

// Draws a badge of exactly `size` pixels square -- the number SizeWithin() gave
// back -- with its top-left corner at (x, y). Any other size is rounded down to
// one this can actually draw.
void Draw(LovyanGFX& canvas, int x, int y, int size);

}
