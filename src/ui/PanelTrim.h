#pragma once

#include <LovyanGFX.hpp>

#include "LGFX.h"

// Correction for a module whose glass sits a degree or two out of true inside
// its round bezel, which reads as every horizontal stroke on the panel -- clock
// segments, text baselines, the progress bar -- running slightly downhill.
//
// The controller cannot help with this. MADCTL turns the scan-out in quarter
// turns only, so anything short of 90 degrees has to be done to the pixels; and
// turning each primitive as it is drawn is not an option either, because the
// screens here are built from text and rounded rectangles that have no shared
// transform to hook into. So it is done once, at the last moment, to whole
// finished frames.
//
// That is what this is: the single place that knows whether an angle is being
// applied, and the reason every full-screen UI composes into a buffer and hands
// it over instead of addressing the panel itself.
//
// Untrimmed is the overwhelmingly common case and costs nothing. Canvas() gives
// back the panel, drawing lands on it directly as it always did, and Present()
// has nothing left to do.
namespace PanelTrim {

// Positive turns the picture clockwise, so a screen whose horizontal strokes
// fall away to the right wants a negative value.
//
// Ignored unless it is a plausible mounting error -- see SCREEN_TRIM_MAX_DEGREES
// in DisplayConfig.h. A live angle also needs a full-screen scratch buffer to
// compose into; if that will not allocate, the trim is dropped rather than the
// screens, and this returns false so the caller can say so on the serial log.
// Call before anything is drawn.
//
bool Begin(float degrees);

// What Begin() settled on, which is zero unless it both liked the angle and got
// its buffer.
float Degrees();

// Where a whole-screen UI should draw: the panel itself when there is nothing
// to correct, the scratch buffer when there is. Whatever was last presented is
// still on it, which is what lets the boot screen paint its logo once and then
// advance a progress bar over it.
LovyanGFX& Canvas(LGFX& tft);

// Puts what has been drawn on Canvas() onto the panel, turned if need be.
void Present(LGFX& tft);

// The radar's own frame. Sent straight, never turned -- see DrawTurnedText for
// why the radar face is the one screen that does not want correcting whole.
void PushFrame(LGFX_Sprite& frame);

// Which place on the radar face a run of turned text comes from. Each gets a
// buffer of its own, sized to its own text and kept: sharing one buffer would
// mean resizing it two or three times a frame as the callers took turns, and
// keying the buffers by call order would resize them all over again whenever
// the wind went stale or the clock changed format.
enum class TextSlot : uint8_t { Wind, ClockDigits, ClockSuffix, Location, Count };

// One run of text, turned by the trim angle, centred on (centreX, centreY) in
// `target`. Font, size and colour come from `target`, exactly as they would for
// the drawCentreString this replaces.
//
// Composed into `target` like any other drawing, so it reaches the panel in the
// same single transfer as the frame it belongs to. It was briefly sent to the
// panel separately, to reach a deeper destination than the 8-bit backbuffer
// then offered; that flickered, because for part of every frame the panel held
// a face with holes in it, and it cost 60ms a frame, because this panel frames
// each command with its own chip-select and every scanline of a blended write
// re-windows. The backbuffer is 16-bit now instead, which is what makes
// anti-aliased text worth compositing there -- see main.cpp.
//
// This is what the radar face uses instead of turning the finished frame, and
// the reason is that a radar face is rotationally symmetric: the range rings
// look identical at every angle, and a sweep going round cannot be three
// degrees out of true in any sense a person could name. Text is the only thing
// on that screen with an up. Turning the whole frame meant resampling 129,600
// pixels to correct the few thousand that needed it -- and resampling the other
// 121,000 for nothing, which is what cost the sweep its smoothness.
//
// Small text is rendered at double size and brought back down on the way in, so
// the one-pixel stems of the built-in face have something to be made out of.
// Without that they would only ever be ragged or blurred, whichever resampler
// they went through.
void DrawTurnedText(LGFX_Sprite& target, TextSlot slot, const char* text, int centreX, int centreY);

// A filled rectangle turned by the trim angle and centred on a turned position:
// the plate a run of text sits on, for the one run that needs the range rings
// cleared out from behind it.
//
// It has to turn with the text it carries. Left upright it announces the trim
// rather than hiding it -- level digits inside a plate still three degrees out,
// cutting a range ring at one height on the left and another on the right.
void FillTurnedPlate(LGFX_Sprite& target, int centreX, int centreY,
                     int width, int height, uint32_t colour);

}
