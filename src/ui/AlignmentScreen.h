#pragma once

#include "LGFX.h"

// The pattern for squaring a radar up with whatever it is mounted in: a
// crosshair to judge level against, a ring on the outermost row of pixels the
// raster has, and a scale around the rim to read the error off in degrees.
//
// Turned on from the configuration page and left on, because setting a trim is
// a look-adjust-look loop and saving the page restarts the radar each time
// round. Turn it off again when the crosshair sits level.
namespace AlignmentScreen {

// Paints once and returns; nothing here animates.
//
// Turned whole, the way the still screens are, and unlike the radar face, which
// turns only the text on it -- rings and a sweep look the same at any angle, but
// a crosshair is the one thing here that has to show the correction bodily. At a
// trim of zero it shows the error as the panel actually has it, and as the trim
// is dialled in you watch the crosshair come level.
//
// What you are judging it against is the reference the radar is mounted in: the
// enclosure, or the PCB's own edge. The ring is no use for it, because a circle
// looks the same at every angle.
void Draw(LGFX& tft);

}
