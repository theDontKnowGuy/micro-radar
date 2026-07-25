# 📡 Micro Radar — ESP32-S3 fork

A tiny live flight radar for a 1.28-inch round display.

> [!IMPORTANT]
> This is a fork of [Anthony Sturdy's Micro Radar](https://github.com/AnthonySturdy/micro-radar). Full credit to Anthony for the excellent original project, firmware, enclosure, and design. This fork adds enhancements; the original project remains Anthony's work.

I built this version with an **ESP32-S3 and a separate 1.28-inch 240 × 240 GC9A01 TFT** because I did not have the combined ESP32-C3/TFT module used by the original project.

![Dense radar scene with collision-aware aircraft labels](docs/images/dense-radar-scene.webp)

## The main improvement: readable dense traffic

Aircraft text now tries to avoid other labels, markers, and the screen edge—even when many aircraft are close together.

The algorithm is a **discrete candidate local search**: it tests positions in eight directions at three distances, then uses iterated coordinate descent to minimize overlap, off-screen text, crossed leader lines, ambiguous ownership, distance, and movement. A pairwise 2-opt repair step handles crossed or swapped label pairs that single-label moves cannot fix.

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
- Knots or metres per second; metres or compact aviation-style feet.
- Improved configuration page with location detection and place search.

## Hardware

- ESP32-S3 DevKitM-1
- Separate 1.28-inch 240 × 240 GC9A01 SPI TFT
- USB data cable
- Jumper wires or soldered connections

Check the voltage requirements of your exact display board before connecting power.

## Wiring

The pin assignment is defined in [`include/LGFX.h`](include/LGFX.h).

| GC9A01 pin | ESP32-S3 |
|---|---:|
| `GND` | `GND` |
| `VCC` | Per display specification |
| `SCL` / `SCLK` | GPIO 11 |
| `SDA` / `MOSI` | GPIO 12 |
| `DC` | GPIO 2 |
| `CS` | GPIO 13 |
| `RST` / `RES` | Not controlled by firmware |
| `BL` / `LED` | Not controlled by firmware |

MISO is not used. If your wiring is different, update `include/LGFX.h`.

## Build and upload

1. Install [VS Code](https://code.visualstudio.com/) and [PlatformIO](https://platformio.org/install/ide?install=vscode).
2. Open this repository in VS Code.
3. Connect the ESP32-S3 with a USB data cable.
4. Run **PlatformIO: Upload**.

The serial monitor runs at `115200` baud. If uploading fails, use your board's BOOT/RESET upload sequence.

## Setup

On first boot, connect to:

```text
MicroRadar-Setup
```

Enter your Wi-Fi details. After the radar restarts, configure it from another device on the same network:

```text
http://microradar.local
```

If mDNS is unavailable, use the IP address shown by the serial monitor or router.

## Configuration

The web page lets you set:

- radar centre and radius;
- current, approximate, or searched location;
- OpenSky client credentials;
- sweep animation and speed;
- aircraft symbol;
- speed, altitude, units, and route labels.

Saving restarts the radar.

An [OpenSky Network](https://opensky-network.org/) account is optional but provides a larger request allowance. Route labels use [ADSBDB](https://www.adsbdb.com/). Place search uses [Nominatim](https://nominatim.org/), and approximate location uses [IPWhoIs](https://ipwhois.io/).

## Enclosure files

The `hardware/` files come from the original project and were designed for its combined ESP32-C3/TFT module. They may need modification for an ESP32-S3 and separate display.

See the [original repository](https://github.com/AnthonySturdy/micro-radar) for Anthony's enclosure and assembly instructions.

## Credits and licence

Original project by [Anthony Sturdy](https://github.com/AnthonySturdy). It was inspired by [therealhacksaw's desk radar](https://www.instagram.com/therealhacksaw/).

Distributed under the [MIT License](LICENSE), with the original copyright notice retained.
