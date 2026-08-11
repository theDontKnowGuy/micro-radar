# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a 2.1" round ESP32-S3 live flight radar (fork of Anthony Sturdy's
Micro Radar, adapted for an ESP32-S3 + separate GC9B72 TFT instead of the
original's combined ESP32-C3/TFT module). Arduino framework, built with
PlatformIO. There is no host-side test suite — correctness is verified by
building, flashing, and watching the serial console / panel.

## Build and upload

There is exactly one PlatformIO environment: `esp32-s3-gc9b72`.

```bash
# Use PlatformIO's own Python, not the project .venv (its esptool breaks the build)
~/.platformio/penv/bin/pio run -e esp32-s3-gc9b72              # build
~/.platformio/penv/bin/pio run -e esp32-s3-gc9b72 -t upload -t monitor  # flash + serial monitor (115200 baud)
```

Only flash this to an ESP32-S3 **N16R8** module (16 MB flash, 8 MB octal
PSRAM) — the 360×360 backbuffer (129,600 bytes) is placed in PSRAM because it
doesn't fit in SRAM alongside the Wi-Fi stack and a TLS handshake, and there's
no PSRAM to put it in on any other module. `board_build.partitions =
partitions_ota.csv` gives two OTA app slots; do not switch this back to
`huge_app.csv`, which has only one slot and makes self-updating impossible.

`maximum_size` is deliberately not set in `platformio.ini` — PlatformIO
derives the real size limit from the OTA app slot in `partitions_ota.csv`.
Setting it to the raw flash size would disable the "firmware too big" check.

### Other scripts

- `scripts/release.sh` — cuts an OTA release: builds the release env(s), tags
  the commit, computes digests, writes `manifest.json`, publishes a GitHub
  release. Version/date/notes are read from `include/FirmwareVersion.h` only
  (never passed on the command line), so edit that header and commit before
  running it. Refuses to publish unless `FIRMWARE_RELEASED` is today's date
  (override with `MICRO_RADAR_ALLOW_STALE_DATE=1`). `MICRO_RADAR_REPO`
  controls the target repo.
- `scripts/pull-coredump.sh` — reads and decodes a panic coredump from flash
  against the build that produced it (`--erase` to clear it after, `--dbg`
  for interactive gdb). Prefer this over the serial console for crashes: panic
  output over native USB CDC can truncate if the host stops draining the
  endpoint, and the coredump in flash is written before that happens.
- `scripts/generate_ca_header.sh` — regenerates `include/UpdateRootCAs.h`
  (the pinned TLS roots for the OTA update channel) from the macOS trust
  store. Re-run only if GitHub rotates certificate issuers.
- `generate_insights.py` — PlatformIO post-build script (wired via
  `extra_scripts` in `platformio.ini`); packages the ESP Insights artifacts
  needed for remote crash symbolication. Not run directly.

## Architecture

### Startup and main loop (`src/main.cpp`)

`setup()` sequence matters and is order-dependent: read stored settings →
draw boot logo → allocate the PSRAM backbuffer → join Wi-Fi (falling back to
a setup-portal access point if there's no stored network or it fails) →
start NTP → start `Diagnostics` (nothing before Wi-Fi join can be reported —
the log hook isn't installed yet) → start the config web server → init
`AircraftManager` and `FirmwareUpdater`.

`loop()` runs at a fixed ~30 FPS cadence (`FRAME_INTERVAL_MS = 33`) and
handles, in priority order: `Diagnostics::Poll()` (self-disables on repeated
transport failures), a pending settings-triggered restart, a pending
firmware install, the alignment/calibration test pattern, then the normal
draw cycle (`RadarSweep::Draw` → `aircraftManager.Draw` → `PanelTrim::PushFrame`).
Restarts and firmware flashes are deliberately deferred to a point in `loop()`
where no SPI DMA transfer to the panel is in flight — doing it from an
HTTP handler or mid-frame corrupts memory on reboot.

Network work (aircraft fetch, wind, timezone, destination lookup, label
layout) never runs inline in `loop()` — it's dispatched to a single
background FreeRTOS task in `AircraftManager` (see below) so the render loop
never blocks on HTTP/TLS.

### AircraftManager (`src/AircraftManager.{h,cpp}`, ~2000 lines)

The core of the radar. Owns the tracked-aircraft map, the label-placement
solver, and a **single background worker task** (`NetworkTaskLoop`) that all
network jobs funnel through one at a time (`NetworkJobType`: FetchAircraft,
FetchWind, FetchTimezone, ResolveDestination, SolveLabels). Only one network
job runs at once by design — this board doesn't have contiguous heap for two
concurrent TLS sessions (a second attempt fails with
`mbedtls_ssl_setup returned -0x7F00` / "SSL - Memory allocation failed").
State handoff between the render loop and the worker task goes through a
mutex (`networkStateMutex`) and a "claim → do work → publish + release
busy flag in one critical section" pattern (`TryClaimNetworkWorker` /
`PublishNetworkResult`), so the render loop can never observe a free worker
whose results aren't visible yet.

**Label placement** (the project's main feature over upstream) lives in
`PlaceAircraftLabels`/`SolveAircraftLabels`, run on the background worker via
the `SolveLabels` job type, not on the render thread. It's a discrete
candidate local search: adjacent positions, short tangential slides, and a
radial fallback are scored for overlap, off-screen text, obstructed/crossed
leader lines, distance, and movement, refined by iterated coordinate descent,
with a pairwise 2-opt repair pass for connector conflicts single-label moves
can't fix. Placement offsets are stored per-aircraft (`TrackedAircraft::labelOffsetX/Y`)
so small position changes don't make labels jump between similar-scoring
choices.

**Aircraft state updates arrive queued, not applied immediately**
(`TrackedAircraft::QueueUpdate`/`ApplyQueuedUpdate`): a fresh network fetch is
held until the radar sweep beam reaches that aircraft's bearing, so aircraft
appear to update as the sweep passes rather than all jumping at once.
Positions between fetches are extrapolated (`PredictPosition`, dead reckoning
from velocity/heading) and eased between the pre- and post-update position
(`GetDisplayPosition`, smoothstep blend) rather than snapping.

### Display pipeline

`LGFX` (LovyanGFX) drives the panel over SPI. `include/LGFX.h` defines the
pin mapping and bus config — SPI clock, MOSI and MISO are deliberately wired
to the ESP32-S3's native IOMUX pins for SPI2 to bypass the GPIO matrix
(80 MHz vs 40 MHz ceiling); see the README's Wiring section before changing
pins. Rendering draws into a PSRAM-backed `LGFX_Sprite` backbuffer at 8-bit
color depth (raising this cost ~4x the frame time for negligible visual
gain — see the comment in `main.cpp`), which `PanelTrim` then pushes to the
panel, applying a screen-trim/rotation correction and controlling backlight
PWM.

UI screens under `src/ui/` (`BootScreen`, `StatusScreen`, `UpdateScreen`,
`AlignmentScreen`, `RadarSweep`, `ProgressBar`, `FrameTimer`, `PanelTrim`)
are each self-contained draw routines called from `main.cpp`/`AircraftManager`,
not a stateful UI framework.

### Configuration (`src/ConfigurationWebServer.{h,cpp}`)

One `AsyncWebServer` instance serves the same configuration page in both
`Mode::Station` (normal operation) and `Mode::Setup` (the radar's own
`MicroRadar-Setup` access point when no Wi-Fi is stored/working — DNS is
hijacked via `DNSServer` for a captive portal). Settings live in NVS via
`Preferences`; `EnsureDefaults()` only fills keys that have never been set, so
defaults never clobber an already-configured radar. Saving settings from the
web UI doesn't restart immediately — it sets an atomic flag
(`RestartRequested()`) that `main.cpp`'s `loop()` checks and acts on between
frames, for the same DMA-safety reason as firmware installs.

### OTA updates (`src/FirmwareUpdater.{h,cpp}`, `include/FirmwareVersion.h`, `include/UpdateRootCAs.h`)

Polls `manifest.json` from the latest GitHub release hourly (first check ~2
min after boot), downloads into the inactive OTA app slot, verifies MD5
against the manifest, then flips the boot slot and reboots — a failed/aborted
download never touches the running firmware. TLS is pinned to two hardcoded
roots (GitHub + githubusercontent.com redirect target) rather than trusting
the system CA store, since this channel can execute arbitrary code on the
device. See the README's "Over-the-air updates" section for the full
publish/version/security model — it's substantial and not duplicated here.

### Remote diagnostics (`src/Diagnostics.{h,cpp}`)

Thin wrapper over ESP Insights (already in the Arduino/IDF core, no new
dependency). Off by default and stays off until an auth key is saved via the
config page — see the header comment in `Diagnostics.h` for the reasoning
(public firmware images, other people's home networks). Use
`Diagnostics::Event/Warn/Error` from firmware code, not `Serial.print`, if a
message should reach the remote dashboard — `Serial` doesn't go through the
IDF logging path the Insights agent taps. Self-disables for the rest of the
boot after 20 transport errors within a minute (a rejected key retries at TLS-handshake
speed, which starves the aircraft fetch / OTA updater of heap otherwise).

### Networking (`src/HttpRequestManager.{h,cpp}`, `src/OpenSkyAuthTokenHandler.{h,cpp}`, `src/WiFiConnection.{h,cpp}`)

`HttpRequestManager` centralizes outbound HTTPS. External services used:
OpenSky Network (aircraft state, optional client credentials for a larger
quota), ADSBDB (route/destination lookup), Open-Meteo (surface wind +
UTC offset for the configured coordinates, since this chip has no timezone
database), Nominatim (place search), IPWhoIs (approximate location by IP).

## Editing conventions specific to this codebase

- Comments here are almost all "why", written for someone who will otherwise
  make the same mistake again (memory constraints, ordering hazards, hardware
  quirks) — match that style rather than describing *what* the code does.
- Anything touching heap/TLS timing should account for the recurring
  constraint in this codebase: **only one concurrent TLS session fits** in the
  heap this board has free (aircraft fetch, firmware download, and the
  diagnostics agent all compete for it). `AircraftManager::SuspendNetworkTask()`
  and `Diagnostics::PauseForUpdate()` exist specifically to serialize this
  during an OTA install.
- Anything that restarts the device or begins a flash write must go through
  the deferred-flag pattern in `main.cpp`'s `loop()` (see
  `ConfigurationWebServer::RestartRequested()` / `FirmwareUpdater::UpdatePending()`),
  never triggered directly from an HTTP handler or mid-frame.
- `include/DisplayConfig.h` holds screen dimensions/inversion; `include/LGFX.h`
  holds pin mapping — check both before assuming panel behavior is fixed.
