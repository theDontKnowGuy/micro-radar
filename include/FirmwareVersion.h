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
#define FIRMWARE_VERSION "1.8.6"

// When this version was published, and what changed in it. Both are shown on
// the configuration page so the running firmware can describe itself, which
// means they have to be compiled in -- the manifest on GitHub describes the
// *latest* release, not necessarily the one a given radar is running.
//
// scripts/release.sh reads both of these too: FIRMWARE_NOTES becomes the
// release notes and the manifest's "notes" field, and the script checks
// FIRMWARE_RELEASED against today's date so a stale one cannot ship.
//
// Keep the date as YYYY-MM-DD and the notes to a sentence -- the notes are
// rendered on a phone-width page.
#define FIRMWARE_RELEASED "2026-08-15"
#define FIRMWARE_NOTES "fix ground planes display"

// Identifies which hardware build this binary is for. Set per environment in
// platformio.ini and used as the key into the manifest's "builds" object, so a
// radar only ever installs an image published for its own hardware.
#ifndef FIRMWARE_BUILD
#define FIRMWARE_BUILD "unknown"
#endif
