#pragma once

#include <LovyanGFX.hpp>

// The QR code for the project's GitHub page, as a badge a screen can put in a
// corner of itself rather than as a screen of its own.
//
// It shares the panel with the address screen because that is the one moment
// the radar has someone's attention and a phone already in their hand -- and
// because a screen that shows nothing but a code is a screen that has to be
// waited out by everyone who has already scanned it once.
//
// Drawn on a LovyanGFX rather than the panel type for the same reason the
// progress bar is: with a rotation trim set it lands in a buffer that PanelTrim
// turns on the way out.
namespace ProjectQr {

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
