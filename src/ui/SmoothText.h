#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

// Text for the screens that are read rather than watched, set from a face far
// larger than the size it is shown at and brought back down by an
// area-averaging resample. What reaches the panel has graded edges instead of
// stepped ones.
//
// The panel UI is set in bitmap faces -- one bit per pixel, a stem is either
// there or it is not -- so every curve in the alphabet arrives as a staircase.
// A unit with a rotation trim set did not show that, and for a reason that had
// nothing to do with its glass being crooked: turning a finished frame
// resamples it, resampling grades the edges, and graded edges are what the eye
// reads as a smooth letter. The screens were being anti-aliased by accident,
// and only on the units that happened to need correcting. This does it on
// purpose, at every trim, on every unit.
//
// The extra material comes from DejaVu72, the largest proportional face
// LovyanGFX ships: 75 pixels of ink height against the 47 the biggest size on
// these screens is shown at, and better than four times the material at the
// smallest. It has to be a genuinely bigger face -- setTextSize on the way in
// would not do, because scaling a bitmap face replicates pixels rather than
// adding any, and there is nothing in a replicated pixel to average.
//
// Not for the radar face. That draws thirty times a second, and PanelTrim's
// DrawTurnedText says what resampling a frame's worth of pixels at that rate
// does to the sweep. The screens here are drawn once and then looked at.
namespace SmoothText {

// What `text` will measure across when drawn `height` tall, so a caller can lay
// itself out before anything is drawn. Zero for an empty run.
//
// `height` is the line's height in the sense fontHeight() means it -- the ink
// extent of the face, ascenders to descenders -- and both this and DrawCentre
// place a run exactly where drawCentreString would have put one that tall.
int Width(LovyanGFX& gfx, const String& text, int height);

// `text` centred on `centreX` with the top of its line box on `top`, drawn
// `height` tall in `colour`.
//
// The composing buffers are taken and given back per run: these screens are
// drawn once each and a status screen has at most seven lines, so holding a
// couple of hundred kilobytes of PSRAM between them would be paying rent on
// something used for a few milliseconds. If a buffer will not allocate the run
// is drawn in the nearest bitmap face instead -- stepped, but there -- which
// leaves `canvas`'s own font changed.
void DrawCentre(LovyanGFX& canvas, const String& text, int centreX, int top,
                int height, uint32_t colour);

}
