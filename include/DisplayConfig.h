#pragma once

// The panel this firmware is built for: a 2.1" 360x360 GC9B72 (env:
// esp32-s3-gc9b72).
//
// Everything that depends on the physical screen is derived from SCREEN_SIZE,
// which keeps the radar drawing code free of hard-coded dimensions. See LGFX.h
// for the matching bus/panel wiring.

constexpr int SCREEN_SIZE = 360;

// The GC9B72 renders colours correctly as-is; inverting washes the display out.
constexpr bool DISPLAY_INVERT = false;

constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

// How far the "Panel alignment" setting is allowed to turn the picture before
// it reaches the panel, in degrees either way.
//
// This exists for one thing only: a module whose glass sits a couple of degrees
// out of true inside its round bezel, which shows up as horizontal strokes --
// the clock's segments, text baselines -- running slightly downhill. It is not
// the knob for a panel mounted a quarter turn round; MADCTL handles those in
// the scan-out for free (see LGFX.h) and there is no reason to pay per frame
// for what the controller does for nothing. Fifteen degrees is well past any
// mounting error and comfortably short of 45, where the two would be
// ambiguous.
constexpr float SCREEN_TRIM_MAX_DEGREES = 15.0f;
