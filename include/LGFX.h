#pragma once

#include <LovyanGFX.hpp>
#include "Bus_GC9B72_SPI.hpp"
#include "Panel_GC9B72.hpp"

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_GC9B72 _panel;
    // Not lgfx::Bus_SPI -- the GC9B72 needs half-duplex write-only transfers,
    // which only ESP-IDF's spi_master provides. See Bus_GC9B72_SPI.hpp.
    lgfx::Bus_GC9B72_SPI _bus;

public:
    LGFX(void)
    {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.freq_write = 20000000; // matches the working ESPHome config
            cfg.spi_mode = 0;
            cfg.pin_mosi = 12; // TFT SDA/MOSI
            cfg.pin_sclk = 11; // TFT SCL/SCLK
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
        setPanel(&_panel);
    }
};
