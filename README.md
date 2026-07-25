# 📡 Micro Radar — ESP32-S3 fork

A tiny, open-source live flight radar for a 1.28-inch round display.

> [!IMPORTANT]
> This repository is a fork of [Anthony Sturdy's original Micro Radar project](https://github.com/AnthonySturdy/micro-radar). Full credit goes to Anthony for the original idea, design, firmware, enclosure, and the great work that made this project possible.

I made this fork because I did not have the combined ESP32-C3 and TFT module used by the original project. My build uses an **ESP32-S3 development board and a separate 1.28-inch, 240 × 240 GC9A01 round TFT**. I then adapted the firmware for that hardware and expanded the radar display, configuration interface, and runtime behaviour.

## Contents

- [How this fork differs](#how-this-fork-differs)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [Build and upload](#build-and-upload)
- [First-time setup](#first-time-setup)
- [Configuration](#configuration)
- [Data sources and privacy](#data-sources-and-privacy)
- [Original enclosure files](#original-enclosure-files)
- [Troubleshooting](#troubleshooting)
- [Credits and licence](#credits-and-licence)

## How this fork differs

### Different hardware

- Ported from the original combined **ESP32-C3 + integrated TFT module** to an **ESP32-S3 DevKitM-1 + separate GC9A01 TFT**.
- Added the SPI pin mapping required by the separate display.
- Enabled native USB CDC support for the ESP32-S3.
- Removed machine-specific upload and monitor ports so PlatformIO can detect the connected board.
- Pinned the PlatformIO platform and library versions for more repeatable builds.

### Smoother radar operation

- Network requests, route lookups, and label layout work run in a background FreeRTOS task instead of blocking the display loop.
- The display is rendered at a steady target of approximately 30 frames per second.
- New OpenSky snapshots are queued and revealed as the radar beam reaches each aircraft. Targets no longer all jump at the same instant.
- Aircraft positions remain latched between sweeps while the existing prediction and interpolation logic keeps movement natural.
- The radar sweep speed is configurable: 2, 5, 10, 18, or 30 seconds per revolution.
- Aircraft outside the usable circular display area are culled so their symbols and labels do not appear through the bezel.

### Clearer aircraft presentation

- Three selectable target symbols:
  - radar block with heading vector;
  - directional triangle;
  - simple dot.
- Callsigns are always displayed and surrounding whitespace from the API is removed.
- Speed and altitude can be enabled independently.
- Speed can be displayed in knots or metres per second.
- Altitude can be displayed in metres or compact aviation-style feet.
- Optional best-effort route labels show `ORIGIN-DESTINATION` when ADSBDB has a match.
- A global label-placement solver reduces label/marker overlap, keeps labels on-screen, avoids crossing leader lines, and keeps placements stable to reduce visual jumping.
- Leader lines connect displaced labels to the correct aircraft.

### Better configuration

- Redesigned responsive configuration page with grouped radar and aircraft-label settings.
- Added precise browser geolocation where the browser allows it.
- Added approximate IP-based location as a fallback for the device's normal local HTTP page.
- Added place, airport, landmark, and address search using a configurable Nominatim-compatible provider.
- Added safe defaults for new and existing installations without overwriting saved settings.
- OpenSky secrets remain masked when the configuration page is loaded and are not replaced by the masked value when other settings are saved.

## Hardware

This fork is configured for:

- ESP32-S3 DevKitM-1;
- separate 1.28-inch round 240 × 240 GC9A01 SPI TFT, without touch;
- USB data cable;
- jumper wires or a soldered connection between the board and display;
- a suitable enclosure or stand.

The ESP32-S3 and display do not need to be sold as one combined module. Check the voltage requirements printed on your particular display board before connecting power.

## Wiring

The current pin assignment is defined in [`include/LGFX.h`](include/LGFX.h).

| GC9A01 display pin | ESP32-S3 connection | Notes |
|---|---:|---|
| `GND` | `GND` | Common ground |
| `VCC` | Per display specification | Use 3.3 V when required by the module; do not assume every breakout accepts the same supply |
| `SCL` / `SCLK` | GPIO 11 | SPI clock |
| `SDA` / `MOSI` | GPIO 12 | SPI data from ESP32-S3 to display |
| `DC` | GPIO 2 | Data/command |
| `CS` | GPIO 13 | Chip select |
| `RST` / `RES` | Not controlled by firmware | LovyanGFX is configured with reset pin `-1`; the module must handle reset or be wired appropriately |
| `BL` / `LED` | Not controlled by firmware | LovyanGFX is configured with backlight pin `-1`; power it as required by the module |

There is no MISO connection because the display is write-only in this build.

If you use different GPIO pins, update the values in `include/LGFX.h` before compiling. Avoid connecting a display supply or backlight pin until you have checked the specifications for your exact breakout board.

## Build and upload

The project uses [PlatformIO](https://platformio.org/) and is configured by `platformio.ini`.

1. Install [Visual Studio Code](https://code.visualstudio.com/) and the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode).
2. Clone or download this repository and open its root folder in VS Code.
3. Connect the ESP32-S3 using a USB cable that supports data.
4. Let PlatformIO install the pinned dependencies.
5. Run **PlatformIO: Upload**, or use the upload arrow in the VS Code status bar.
6. Open the serial monitor at `115200` baud if you need startup or network diagnostics.

If uploading does not start, hold **BOOT**, briefly press **RESET**, start the upload, and release **BOOT** when PlatformIO begins connecting. The exact sequence can vary between ESP32-S3 boards.

The firmware is compiled as GNU++17 and uses the `huge_app.csv` partition layout.

## First-time setup

On first boot, the radar creates a Wi-Fi access point named:

```text
MicroRadar-Setup
```

Connect to it from a phone or computer. The captive configuration page should open automatically; if it does not, open the sign-in notification or browse to the gateway page offered by the device. Enter the Wi-Fi credentials and save them. The ESP32-S3 will restart and join that network.

After it connects, open:

```text
http://microradar.local
```

The computer or phone must be on the same local network. If mDNS is unavailable on the network, find the ESP32-S3's IP address in the serial monitor or router and open that address directly.

## Configuration

Saving the configuration restarts the radar so all settings take effect.

### Location and range

- **Latitude and longitude** set the centre of the radar.
- **Radar radius** controls the displayed area in degrees.
- **Use my current location** uses precise browser geolocation when the page is in a secure context.
- On normal local HTTP, **Use approximate location** estimates coordinates from the public IP address.
- **Place search** finds an airport, city, landmark, or address and fills its coordinates.
- **Geocoder provider URL** can point to a compatible Nominatim service.

### OpenSky

The radar works anonymously, but an [OpenSky Network](https://opensky-network.org/) account is recommended because authenticated access has a larger request allowance. Enter the OpenSky client ID and client secret on the configuration page.

The firmware calculates its polling interval from the anonymous or authenticated daily request allowance and leaves a small token buffer to reduce the chance of exhausting it.

### Radar appearance

- Enable or disable the animated sweep.
- Select a sweep period of 2, 5, 10, 18, or 30 seconds.
- Select a radar block and vector, aircraft triangle, or simple dot.

### Aircraft labels

- Callsigns are always shown.
- Speed is optional and can use knots (`kt`) or metres per second (`m/s`).
- Altitude is optional and can use metres or compact feet, for example `456ft`, `3.4Kft`, or `14Kft`.
- Route is optional and is displayed only when a match is available.

## Data sources and privacy

This firmware can contact the following services:

- [OpenSky Network](https://opensky-network.org/) for live aircraft state vectors;
- [ADSBDB](https://www.adsbdb.com/) for optional callsign-to-route lookup;
- [OpenStreetMap Nominatim](https://nominatim.org/) or the configured compatible provider when place search is used;
- [IPWhoIs](https://ipwhois.io/) when approximate location is requested;
- jsDelivr when the browser loads the configuration page's Tailwind CSS script.

Precise browser location is used to fill the form in the browser. Approximate IP location and place search send a request to their respective public services. A VPN, mobile carrier, or distant ISP gateway can make IP-based coordinates inaccurate. Route data is best-effort and can be unavailable, stale, or ambiguous.

## Original enclosure files

The `hardware/` directory is retained from the upstream project and includes STL, 3MF, and Onshape resources created for the original combined ESP32-C3/TFT module.

Those parts may **not** fit an ESP32-S3 board and separate display without modification. They are included as a useful starting point and remain part of Anthony Sturdy's original work. Check all dimensions before printing or edit the design for your exact ESP32-S3, display breakout, USB connector, and mounting method.

Refer to the [original Micro Radar repository](https://github.com/AnthonySturdy/micro-radar) for the original integrated-board shopping list and detailed enclosure assembly instructions.

## Troubleshooting

### The screen is blank

- Confirm that the display is a GC9A01 240 × 240 SPI model.
- Recheck ground, power, GPIO 11 (`SCL`), GPIO 12 (`SDA`), GPIO 2 (`DC`), and GPIO 13 (`CS`).
- Check whether the display's `RST` and `BL` pins must be tied high or supplied separately.
- Verify the display voltage requirements before changing the power connection.
- If colours are wrong, the display may require the RGB-order option in `include/LGFX.h`.

### The upload port is missing or busy

- Disconnect and reconnect the board, then restart VS Code.
- Make sure the USB cable supports data.
- Select the detected serial port in PlatformIO.
- Close any other serial monitor that is using the port.
- Try the BOOT/RESET upload sequence described above.

### `microradar.local` does not open

- Confirm that the radar and browser are on the same network.
- Check the serial monitor for the connection status.
- Find the device IP address in the router and use it instead.
- Some guest networks isolate clients and block both direct access and mDNS.

### No aircraft appear

- Save valid latitude, longitude, and radius values.
- Check that Wi-Fi has internet access.
- Look at the serial monitor for OpenSky request or authentication errors.
- Try anonymous access if the saved OpenSky credentials are incorrect.
- Confirm that aircraft are currently inside the selected area.

### Place search, approximate location, or page styling does not work

These features depend on public internet services. Check internet access, DNS, browser privacy blocking, and the configured geocoder URL. Coordinates can always be entered manually.

## Credits and licence

This is a fork of [Micro Radar](https://github.com/AnthonySturdy/micro-radar), originally designed and developed by [Anthony Sturdy](https://github.com/AnthonySturdy). Thank you to Anthony for the excellent original project and for releasing it as open source.

The original project was inspired by [therealhacksaw's desk radar](https://www.instagram.com/therealhacksaw/).

The project is distributed under the [MIT License](LICENSE). The original copyright and licence notice are retained.
