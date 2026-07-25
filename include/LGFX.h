#pragma once

#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI _bus;

public:
    LGFX(void)
    {
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
        setPanel(&_panel);
    }
};
