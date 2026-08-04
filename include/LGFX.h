#pragma once

#include <LovyanGFX.hpp>

#include "DisplayConfig.h"

#if defined(PANEL_GC9B72)
#include "Bus_GC9B72_SPI.hpp"
#include "Panel_GC9B72.hpp"
#endif

class LGFX : public lgfx::LGFX_Device
{
#if defined(PANEL_GC9B72)
    lgfx::Panel_GC9B72 _panel;
    // Not lgfx::Bus_SPI -- the GC9B72 needs half-duplex write-only transfers,
    // which only ESP-IDF's spi_master provides. See Bus_GC9B72_SPI.hpp.
    lgfx::Bus_GC9B72_SPI _bus;
#else
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI _bus;
#endif

public:
    LGFX(void)
    {
#if defined(PANEL_GC9B72)

        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            // Frame rate here is bound by how fast a 360x360 sprite (259,200
            // bytes) can be pushed, so the SPI clock directly sets how smooth
            // the radar sweep looks.
            //
            // These pins are deliberately the ESP32-S3's native IOMUX pins for
            // SPI2 (FSPICLK=12, FSPID=11). Matching them lets ESP-IDF drive the
            // bus straight through IOMUX instead of the GPIO matrix, which
            // raises the ceiling from 40MHz to 80MHz. Swapping SCLK/MOSI to any
            // other pins silently drops back to 40MHz max.
            //
            // 80MHz is demanding over jumper wires. If pixels come out garbled,
            // step down: 60000000, then 50000000, then 40000000.
            cfg.freq_write = 80000000;
            cfg.spi_mode = 0;
            cfg.pin_mosi = 11; // TFT SDA/MOSI  -> S3 native FSPID
            cfg.pin_sclk = 12; // TFT SCL/SCLK  -> S3 native FSPICLK
            cfg.pin_dc = 2;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 13;
            cfg.pin_rst = 6; // TFT RST/RES
            cfg.pin_busy = -1;
            // The GC9B72 wants MADCTL = 0x00 (RGB). LovyanGFX ORs in MAD_BGR
            // (0x08) unless rgb_order is set, which does not match the working
            // ESPHome config for this panel.
            cfg.rgb_order = true;
            cfg.readable = false; // no SDO wired
            _panel.config(cfg);
        }

#else

        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.freq_write = 27000000;
            cfg.pin_miso = -1;
            cfg.pin_mosi = 12; // TFT SDA/MOSI
            cfg.pin_sclk = 11; // TFT SCL/SCLK
            cfg.pin_dc = 2;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 13;
            cfg.pin_rst = -1;
            cfg.pin_busy = -1;
            // cfg.rgb_order = true;
            _panel.config(cfg);
        }

#endif

        setPanel(&_panel);
    }
};
