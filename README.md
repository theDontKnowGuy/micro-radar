# 📡 Micro Radar — ESP32-S3 fork

A tiny live flight radar for a 2.1-inch round display.

> [!IMPORTANT]
> This is a fork of [Anthony Sturdy's Micro Radar](https://github.com/AnthonySturdy/micro-radar). Full credit to Anthony for the excellent original project, firmware, enclosure, and design. This fork adds enhancements; the original project remains Anthony's work.

I built this version with an **ESP32-S3 and a separate 2.1-inch 360 × 360 GC9B72 TFT** because I did not have the combined ESP32-C3/TFT module used by the original project.

![Dense radar scene with collision-aware aircraft labels](docs/images/dense-radar-scene.webp)

## The main improvement: readable dense traffic

Aircraft text now tries to avoid other labels, markers, and the screen edge—even when many aircraft are close together.

The algorithm is a **discrete candidate local search**: it tests adjacent positions, short tangential slides, and a radial fallback, then uses iterated coordinate descent to balance overlap, off-screen text, obstructed or crossed leader lines, distance, ownership, and movement. A pairwise 2-opt repair step handles connector conflicts or swapped label pairs that single-label moves cannot fix.

It:

- moves labels to reduce overlap;
- keeps labels inside the round display;
- uses leader lines to connect moved labels to their aircraft;
- avoids crossed leader lines where possible;
- keeps placements stable so text does not constantly jump around.

## Other improvements

- Network and layout work run in the background for a smoother display.
- Aircraft updates appear as the radar sweep reaches each target instead of all jumping at once.
- Configurable sweep speed: 2, 5, 10, 18, or 30 seconds per revolution.
- Three aircraft symbols: radar vector, directional triangle, or dot.
- Optional speed, altitude, and `ORIGIN-DESTINATION` route labels.
- Optional aviation-style surface-wind readout for the configured radar centre.
- Knots or metres per second; metres or compact aviation-style feet.
- Improved configuration page with location detection and place search.

## Hardware

- ESP32-S3 DevKitM-1, **N16R8** module — 16 MB flash and 8 MB octal PSRAM
- A 2.1-inch 360 × 360 GC9B72 SPI TFT
- USB data cable
- Jumper wires or soldered connections

The panel needs a 129,600-byte framebuffer, which does not fit in SRAM alongside
the Wi-Fi stack and a TLS handshake. On an N16R8 module the firmware puts it in
PSRAM automatically; on a board without PSRAM, OpenSky requests fail with
`SSL - Memory allocation failed`.

Check the voltage requirements of your exact display board before connecting power.

## Wiring

The pin assignment is defined in [`include/LGFX.h`](include/LGFX.h).

| Display pin | GC9B72 (2.1") |
|---|---:|
| `GND` | `GND` |
| `VCC` | Per display specification |
| `SCL` / `SCLK` | GPIO 12 |
| `SDA` / `MOSI` | GPIO 11 |
| `DC` | GPIO 3 |
| `CS` | GPIO 13 |
| `RST` / `RES` | GPIO 6 |
| `BL` / `LED` | GPIO 17 |

`MISO`/`SDO` and `TE` are not used.

The backlight is PWM-driven rather than tied to `3V3`, so the panel can be dimmed
and stays dark until the first frame is on screen. If your display board exposes
`BL` as a raw LED anode instead of a logic-level enable, switch it with a
transistor rather than driving it from the GPIO.

> [!IMPORTANT]
> `SCL` on GPIO 12 and `SDA` on GPIO 11 is deliberate, and looks backwards next
> to most wiring guides. GPIO 12 and 11 are the ESP32-S3's native IOMUX pins for
> SPI2 (`FSPICLK` and `FSPID`), and matching them lets the bus bypass the GPIO
> matrix. That raises the SPI ceiling from 40 MHz to 80 MHz, which roughly halves
> the time to push a 360 × 360 frame and is the difference between a jumpy and a
> smooth radar sweep.

If your wiring is different, update `include/LGFX.h`. Keep off GPIO 26–32 (SPI
flash), 33–37 (the octal PSRAM this build requires), 19/20 (native USB, which
carries the serial console here), and 0/45/46 (strapping). GPIO 3 is also a
strapping pin, but it only selects the JTAG signal source and is safe to drive.

## Build and upload

1. Install [VS Code](https://code.visualstudio.com/) and [PlatformIO](https://platformio.org/install/ide?install=vscode).
2. Open this repository in VS Code.
3. Connect the ESP32-S3 with a USB data cable.
4. Run **PlatformIO: Upload** with the `esp32-s3-gc9b72` environment, the only
   one this project defines.

Do not flash it to a board without octal PSRAM; it configures the module for
16 MB flash and 8 MB OPI PSRAM. Screen dimensions live in
[`include/DisplayConfig.h`](include/DisplayConfig.h).

From the command line:

```bash
pio run -e esp32-s3-gc9b72 -t upload -t monitor
```

The serial monitor runs at `115200` baud. If uploading fails, use your board's BOOT/RESET upload sequence.

> **Upgrading from a build before over-the-air updates:** the partition table
> changed from `huge_app.csv` to [`partitions_ota.csv`](partitions_ota.csv), and
> flashing a new partition table erases NVS. The first serial upload after this
> change wipes saved Wi-Fi credentials and every setting from the web UI, so the
> radar comes back up in setup-hotspot mode. This happens once — later OTA
> updates only rewrite an app slot and leave settings alone.
>
> You may also see this once per boot afterwards:
>
> ```text
> E (253) esp_core_dump_flash: Incorrect size of core dump image: 1819042143
> ```
>
> The coredump partition moved to `0x7F0000`, a region that previously held
> leftover data, and the core dump component is reading those stale bytes as a
> length. Nothing is broken — it just means there is no valid crash dump stored
> — but you can silence it permanently by erasing that region once. An erased
> partition is skipped without complaint:
>
> ```bash
> esptool.py --chip esp32s3 --port /dev/cu.usbmodem11201 erase_region 0x7F0000 0x10000
> ```
>
> Use `erase_region`, not `erase_flash` — the latter would clear your Wi-Fi
> credentials and settings along with it.

## Over-the-air updates

The radar checks GitHub for new firmware once an hour and, by default, installs
anything newer on its own. No button to press and nothing to upload: publish a
release and the fleet picks it up within the hour. A radar set to **Ask me
first** on its configuration page finds the release on the same schedule but
waits for you to start the install.

**How it works.** Flash holds two app slots. The running firmware downloads a
new image into the slot it is *not* executing from, checks it against the MD5
published in the manifest, and only then points the bootloader at it and
reboots. A download interrupted by a dropped Wi-Fi connection, a truncated
transfer, or a bad digest leaves the running firmware completely untouched — the
worst case is that the radar reboots on the version it already had and tries
again an hour later.

The device polls `manifest.json`, a small file attached to the latest release:

```json
{
  "version": "1.1.0",
  "notes": "Shows destination airport for arriving flights",
  "released": "2026-08-05",
  "builds": {
    "esp32-s3-gc9b72": { "url": "...", "md5": "...", "size": 1318096 }
  }
}
```

Each board only installs the image published under its own build key, so a
binary built for different hardware is never offered to it.

### Which version am I running?

Each radar reports its own build in two places. The boot screen shows the bare
version under the configuration address:

```text
Configure me at
http://192.168.1.42
v1.1.0
```

The configuration page carries the fuller story at the bottom — version,
release date, and what changed:

> **Firmware 1.1.0** · released 2026-08-05
> Adds automatic over-the-air updates from GitHub.

These describe the build that is *running*, not the newest one published, which
is the distinction that matters once units start updating themselves at
different times.

Next to them is a **Check for updates now** link, for when you have just
published a release and do not want to wait out the hour. It reports back in
place:

```text
Check for updates now - Up to date - 1.1.1 is the latest release
Check for updates now - Version 1.2.0 found - installing now
Check for updates now - Downloading 1.2.0 - 45 percent
```

If it finds something, the install starts immediately and the page loses the
radar when it reboots — that is the expected ending, and the status says so.
Reload once it is back to confirm the new version.

### Automatic or manual

The **Firmware updates** section of the configuration page decides what happens
when a check finds a newer release:

- **Install automatically** (default) — the radar downloads and installs it the
  moment it finds it, then reboots. Unattended units stay current on their own.
- **Ask me first** — the radar records the release and stops there. A small
  amber `Update available` line appears above the location name on the radar
  itself, and the configuration page offers an **Install now** button next to
  the version footer, with the status reading `Version 1.2.0 is ready to
  install`. Both persist for as long as the radar stays up, so one that found
  something overnight is still showing it in the morning. A reboot clears the
  reminder, but the check that runs a couple of minutes after boot finds the
  same release again and puts it back.

Installing takes about a minute, during which the display shows a progress bar
instead of aircraft, which is the reason to hold it until a convenient moment.
The setting is stored with the rest of the configuration and takes effect from
the reboot that saving triggers.

### Publishing a release

1. Edit the three constants in [`include/FirmwareVersion.h`](include/FirmwareVersion.h) and commit:

```c
#define FIRMWARE_VERSION  "1.1.0"
#define FIRMWARE_RELEASED "2026-08-05"
#define FIRMWARE_NOTES    "Adds automatic over-the-air updates from GitHub."
```

2. Run it with no arguments:

```bash
scripts/release.sh
```

That builds every release environment, tags the commit, computes the digests,
writes the manifest, and publishes it all as a GitHub release. Everything is
read back out of the header rather than passed on the command line, so what is
compiled into the binary and what the manifest advertises cannot drift apart —
and the radar can describe itself offline. The script refuses to publish if
`FIRMWARE_RELEASED` is not today's date; set `MICRO_RADAR_ALLOW_STALE_DATE=1`
to override. Keep the notes free of double quotes: the value is a C string
literal that the script extracts by matching to the closing quote.

Set `MICRO_RADAR_REPO` to publish somewhere other than the default fork.
Because GitHub resolves "latest" to the newest release that is neither a draft
nor a prerelease, **marking a release as a prerelease is how you stage a build
without shipping it to every radar in the field**.

### Security

The update channel can execute arbitrary code on the device, so it does not
trust the internet's CAs at large. TLS is pinned to the two roots in
[`include/UpdateRootCAs.h`](include/UpdateRootCAs.h) — one covering `github.com`
and one covering `*.githubusercontent.com`, which the release-asset redirect
lands on. Downloads are refused from any other host, and the image must match
the manifest's MD5 before it is allowed to boot.

If GitHub ever changes certificate issuers, update checks start failing with a
TLS error in the serial log and the radar keeps running its current firmware.
Re-pin with:

```bash
scripts/generate_ca_header.sh
```

### Watching it happen

Update activity is logged over serial with an `[OTA]` prefix:

```text
[OTA] Running 1.0.0 (build esp32-s3-gc9b72)
[OTA] Update available: 1.0.0 -> 1.1.0 (1318096 bytes)
[OTA] 1.1.0 written and verified
```

The first check runs two minutes after boot, then hourly. During an install the
panel shows a progress bar and the radar stops tracking aircraft until it
reboots.

## Setup

With no network stored — on first boot, or after the stored one stops answering —
the radar puts up its own hotspot:

```text
MicroRadar-Setup
```

Connect to it and the configuration page opens by itself; if your phone does not
offer it, browse to the address shown on the radar's screen (`192.168.4.1`).

It is the same page you get later over your own network, so the Wi-Fi details,
the radar centre, the OpenSky credentials and the display options are all set in
one go. Saving joins the network and starts the radar. Anything that needs the
internet — place search, current location, the update check — is shown disabled
until then, because the hotspot has no route to it.

Afterwards the radar shows a *Configure me at* screen with its address, and the
same page is reachable from any device on the network:

```text
http://microradar.local
```

If mDNS is unavailable, use the IP address shown on screen, by the serial monitor, or by your router.

A wrong password just brings the hotspot back on the next boot, with everything
else you entered still stored.

## Configuration

![Micro Radar configuration page](docs/images/configuration-page.png)

The web page lets you set:

- the Wi-Fi network, chosen from a scan or typed in if it is hidden;
- radar centre and radius;
- current, approximate, or searched location;
- short location name shown at the bottom of the display;
- OpenSky client credentials;
- sweep animation and speed;
- aircraft symbol;
- center surface-wind readout;
- speed, altitude, units, and route labels.

When the surface-wind display is enabled, it uses an aviation-style readout. For example, `WND 32025G30KT` means wind from 320° at 25 knots, gusting to 30 knots.

The Wi-Fi section also reports the address, signal, MAC, uptime and free memory,
and can forget the stored network — which restarts the radar into its setup
hotspot — or simply restart it.

Saving restarts the radar.

A device with no stored settings starts centred on Ben Gurion Airport
(32.002714, 34.880919), with the surface-wind readout and route labels both on,
so the first boot shows real traffic before anything is configured. These defaults
apply only to keys that have never been set — an already-configured radar keeps
what it has, and a setting turned off stays off.

An [OpenSky Network](https://opensky-network.org/) account is optional but provides a larger request allowance. Route labels use [ADSBDB](https://www.adsbdb.com/), and center surface wind uses [Open-Meteo](https://open-meteo.com/). Place search uses [Nominatim](https://nominatim.org/), and approximate location uses [IPWhoIs](https://ipwhois.io/).

## Enclosure files

The `hardware/` files come from the original project and were designed for its combined ESP32-C3/TFT module. They may need modification for an ESP32-S3 and separate display.

See the [original repository](https://github.com/AnthonySturdy/micro-radar) for Anthony's enclosure and assembly instructions.

## Credits and licence

Original project by [Anthony Sturdy](https://github.com/AnthonySturdy). It was inspired by [therealhacksaw's desk radar](https://www.instagram.com/therealhacksaw/).

Distributed under the [MIT License](LICENSE), with the original copyright notice retained.
