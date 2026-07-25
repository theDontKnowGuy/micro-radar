#pragma once

#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX(void)
    {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.freq_write = 27000000;
            cfg.pin_miso = -1; // no MISO leg available
            cfg.pin_mosi = 12; // SDA per user
            cfg.pin_sclk = 11; // SCL per user
            cfg.pin_dc = 2;    // keep DC on GPIO2
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 13;   // CS per user
            cfg.pin_rst = -1; // leave reset unconnected by default
            cfg.pin_busy = -1;
            // cfg.rgb_order = true;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = -1; // no backlight leg available
            cfg.invert = false;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};