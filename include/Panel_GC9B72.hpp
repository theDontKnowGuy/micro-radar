#pragma once

// LovyanGFX has no built-in driver for the GalaxyCore GC9B72 (2.1" 360x360
// round SPI TFT). The GC9B72 shares the same CASET/RASET/RAMWR addressing
// and MADCTL bit layout as the GC9A01, so we reuse LovyanGFX's Panel_GC9xxx
// base (from Panel_GC9A01.hpp) and only swap in the GC9B72's own init
// sequence and resolution.
//
// Init sequence ported from the xboot project's fb-gc9b72.c reference
// driver (via https://github.com/MaliosDark/Arduino_GC9B72).
#include <lgfx/v1/panel/Panel_GC9A01.hpp>

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

  struct Panel_GC9B72 : public Panel_GC9xxx
  {
    Panel_GC9B72(void)
    {
      _cfg.panel_width  = _cfg.memory_width  = 360;
      _cfg.panel_height = _cfg.memory_height = 360;

      _cfg.dummy_read_pixel = 16;
    }

    // LovyanGFX's stock Panel_LCD::init() holds CS asserted for the *entire* init
    // sequence (one startWrite/endWrite around the whole list). The GC9B72 will not
    // accept its init that way and stays blank.
    //
    // ESPHome's mipi_spi driver -- which is confirmed working on this exact panel --
    // instead frames every command individually: CS low, command byte, CS high, then
    // (if the command has arguments) CS low, data bytes, CS high. The controller
    // latches on those CS edges. The GC9A01 tolerated CS being held low, which is why
    // the original 1.28" panel worked without this.
    //
    // See esphome/components/mipi_spi/mipi_spi.h, write_command_() BUS_TYPE_SINGLE.
    bool init(bool use_reset) override
    {
      if (!Panel_Device::init(use_reset)) { return false; }

      // The GC9B72 needs ~120ms after reset before it will accept SLPOUT.
      // Panel_Device::init() only settles for ~64ms, so top it up.
      delay(120);

      _bus->beginTransaction();
      for (uint8_t i = 0; auto cmds = getInitCommands(i); i++)
      {
        send_init_list(cmds);
      }
      _bus->endTransaction();

      return true;
    }

    // Panel_GC9xxx::setWindow caches the address window and skips CASET/RASET
    // when it is unchanged. Every frame here pushes the same full-screen
    // window, so after the first frame only RAMWR would be sent -- meaning a
    // single corrupted transfer could leave the panel's window wrong with
    // nothing to ever restore it, and the image stays shifted permanently.
    //
    // Invalidating the cache costs 8 bytes per frame and makes a glitch cost
    // one bad frame instead of every frame after it.
    void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye) override
    {
      _xs = _ys = _xe = _ye = INT16_MAX;
      Panel_GC9xxx::setWindow(xs, ys, xe, ye);
    }

  protected:

    // Sends one command (and its argument byte) with the same per-command CS
    // framing the panel requires everywhere else.
    void framed_cmd(uint8_t cmd, uint8_t arg)
    {
      cs_control(false);
      writeCommand(cmd, 1);
      _bus->flush();
      cs_control(true);

      cs_control(false);
      writeData(arg, 1);
      _bus->flush();
      cs_control(true);
    }

    // Panel_LCD::update_madctl() emits COLMOD and MADCTL with CS held low for
    // the whole pair. Those two commands carry the colour format and so decide
    // whether anything is visible at all, so they need the same framing as the
    // init sequence.
    void update_madctl(void) override
    {
      if (_bus == nullptr) { return; }

      startWrite();
      framed_cmd(CMD_COLMOD, getColMod(_write_bits));
      framed_cmd(CMD_MADCTL, getMadCtl(_internal_rotation) | (_cfg.rgb_order ? MAD_RGB : MAD_BGR));
      endWrite();
    }

    // Same encoding as Panel_Device::command_list() (cmd, len[|CMD_INIT_DELAY],
    // args..., [delay_ms]), but with per-command CS framing as described above.
    void send_init_list(const uint8_t* addr)
    {
      for (;;)
      {
        uint8_t cmd = *addr++;
        uint8_t num = *addr++;
        if (cmd == 0xFF && num == 0xFF) break;

        uint_fast8_t ms = num & CMD_INIT_DELAY;
        num &= ~CMD_INIT_DELAY;

        cs_control(false);
        writeCommand(cmd, 1);
        _bus->flush();
        cs_control(true);

        if (num)
        {
          cs_control(false);
          do
          {
            writeData(*addr++, 1);
          } while (--num);
          _bus->flush();
          cs_control(true);
        }

        if (ms)
        {
          ms = *addr++;
          delay(ms == 255 ? 500 : ms);
        }
      }
    }

    const uint8_t* getInitCommands(uint8_t listno) const override
    {
      static constexpr uint8_t list0[] = {
          0xFE, 0,
          0xEF, 0,
          0x80, 1, 0x19,
          0x82, 1, 0x09,
          0x83, 1, 0x03,
          0x88, 1, 0x00,
          0x89, 1, 0x38,
          0x8A, 1, 0x40,
          0x8B, 1, 0x0A,
          0x8C, 1, 0x00,
          0x81, 1, 0xFF,
          0x84, 1, 0xFF,
          0x85, 1, 0xFF,
          0x86, 1, 0xFF,
          0x87, 1, 0xFF,
          0x8E, 1, 0xFF,
          0x8F, 1, 0xFF,
          0x98, 1, 0x3E,
          0x99, 1, 0x3E,
          0x7D, 1, 0x72,
          0x70,10, 0x02, 0x03, 0x03, 0x06, 0x03, 0x03, 0x09, 0x07, 0x09, 0x03,
          0x90, 4, 0x06, 0x06, 0x01, 0x01,
          0x93, 3, 0x02, 0xFF, 0x00,
          0xCB, 1, 0x02,
          0xFB, 2, 0x00, 0x00,
          0xF6, 1, 0xC0,
          0x6C, 7, 0x00, 0x00, 0x22, 0x00, 0xCC, 0x04, 0x58,
          0xAA, 2, 0x0B, 0x00,
          0xEC, 1, 0x07,
          0xF9, 1, 0x40,
          0xEB, 2, 0x01, 0x67,
          0x74, 6, 0x01, 0x60, 0x00, 0x00, 0x00, 0x00,
          0xB5, 3, 0x14, 0x14, 0x14,
          0x6E,32, 0x0B, 0x0B, 0x09, 0x09, 0x13, 0x13, 0x11, 0x11,
                   0x16, 0x15, 0x01, 0x04, 0x00, 0x0D, 0x1D, 0x00,
                   0x00, 0x1D, 0x0D, 0x00, 0x04, 0x08, 0x15, 0x16,
                   0x12, 0x12, 0x14, 0x14, 0x0A, 0x0A, 0x0C, 0x0C,
          0x60, 4, 0x38, 0x1C, 0x13, 0x56,
          0x61, 4, 0xF8, 0x0A, 0x13, 0x56,
          0x62, 4, 0xF8, 0x0B, 0x13, 0x56,
          0x63, 4, 0x38, 0x1C, 0x13, 0x56,
          0x64, 6, 0x38, 0x20, 0x72, 0xF8, 0x13, 0x56,
          0x65, 6, 0x78, 0x1A, 0x70, 0x0B, 0x56, 0x13,
          0x66, 6, 0x38, 0x24, 0x72, 0xFC, 0x13, 0x56,
          0x68, 7, 0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A,
          0x69, 7, 0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A,
          0x6A, 2, 0x00, 0x00,
          0x3A, 1, 0x55,       // COLMOD: 16bpp. 0x55, NOT 0x05 -- the working
                               // ESPHome config sends 0x55 and 0x05 leaves the
                               // RGB interface unset on this controller.
          0x7C, 2, 0xB6, 0x29,
          0xAC, 1, 0x40,
          0xC3, 1, 0x1A,
          0xC4, 1, 0x24,
          0xC9, 1, 0x2F,
          0xF0, 6, 0x11, 0x17, 0x08, 0x06, 0x05, 0x38,
          0xF1, 6, 0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F,
          0xF2, 6, 0x11, 0x17, 0x08, 0x06, 0x05, 0x38,
          0xF3, 6, 0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F,
          0xB4, 1, 0x0A,
          0x35, 1, 0x00,       // Tearing Effect Line ON
          0xFE, 0,
          0xEE, 0,
          0x11, 0+CMD_INIT_DELAY, 120,  // sleep out
          0x29, 0+CMD_INIT_DELAY, 20,   // display on
          0xFF,0xFF, // end
      };
      switch (listno) {
      case 0: return list0;
      default: return nullptr;
      }
    }
  };

//----------------------------------------------------------------------------
 }
}
