#pragma once

// Which panel this firmware is built for. Selected by a build flag in
// platformio.ini so the two supported screens share one codebase:
//
//   -DPANEL_GC9B72   2.1"  360x360  GC9B72   (env: esp32-s3-gc9b72)
//   -DPANEL_GC9A01   1.28" 240x240  GC9A01   (default)
//
// Everything that depends on the physical screen is derived from SCREEN_SIZE,
// which keeps the radar drawing code panel-agnostic. See LGFX.h for the
// matching bus/panel wiring.

#if defined(PANEL_GC9B72)

constexpr int SCREEN_SIZE = 360;
// The GC9B72 renders colours correctly as-is; inverting washes the display out.
constexpr bool DISPLAY_INVERT = false;

#else

constexpr int SCREEN_SIZE = 240;
// The GC9A01 panels used here are inverted relative to the framebuffer.
constexpr bool DISPLAY_INVERT = true;

#endif

constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);
