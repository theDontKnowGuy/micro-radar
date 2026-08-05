#pragma once

// Single source of truth for the version of the firmware being built.
//
// Bump this before cutting a release. scripts/release.sh reads the value back
// out of this file and refuses to publish if it does not match the tag you
// asked for, so the number baked into the binary and the number in the manifest
// can never drift apart.
//
// Format is MAJOR.MINOR.PATCH. FirmwareUpdater compares the three components
// numerically rather than as text, so 1.10.0 correctly sorts above 1.9.0.
#define FIRMWARE_VERSION "1.0.0"

// Identifies which hardware build this binary is for. Set per environment in
// platformio.ini and used as the key into the manifest's "builds" object, so
// one manifest can serve every panel variant and a GC9B72 image is never
// offered to a GC9A01 board.
#ifndef FIRMWARE_BUILD
#define FIRMWARE_BUILD "unknown"
#endif
