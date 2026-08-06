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
