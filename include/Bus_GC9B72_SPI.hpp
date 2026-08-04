#pragma once

// A LovyanGFX bus that talks to the panel through ESP-IDF's spi_master instead
// of LovyanGFX's own register-level SPI driver.
//
// The GC9B72 only responds to half-duplex, write-only SPI transfers. This was
// established on hardware: bit-banged GPIO works, ESP-IDF spi_master configured
// with SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY works at 20MHz, while both
// Arduino's SPIClass and LovyanGFX's Bus_SPI leave the panel blank at every
// clock rate from 1MHz to 40MHz. The settings below are copied from the user's
// known-working ESPHome config (esphome/components/spi/spi_esp_idf.cpp).
//
// CS is deliberately NOT handed to the driver (spics_io_num = -1) -- Panel_GC9B72
// drives it via cs_control() so each command keeps its own CS window, exactly as
// ESPHome does.

#include <algorithm>

#include <lgfx/v1/Bus.hpp>
#include <lgfx/v1/misc/pixelcopy.hpp>

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

  class Bus_GC9B72_SPI : public IBus
  {
    static constexpr uint32_t MAX_CHUNK = 4096;

  public:
    struct config_t
    {
      spi_host_device_t spi_host = SPI2_HOST;
      int16_t pin_sclk = -1;
      int16_t pin_mosi = -1;
      int16_t pin_dc = -1;
      uint32_t freq_write = 20000000;
      uint8_t spi_mode = 0;
    };

    const config_t& config(void) const { return _cfg; }
    void config(const config_t& cfg) { _cfg = cfg; }

    bus_type_t busType(void) const override { return bus_type_t::bus_spi; }

    bool init(void) override
    {
      if (_spi) { return true; }

      // Note: ESP-IDF's gpio_* API, not Arduino's pinMode/digitalWrite -- inside
      // namespace lgfx those names resolve to LovyanGFX's own overloads.
      gpio_set_direction((gpio_num_t)_cfg.pin_dc, GPIO_MODE_OUTPUT);
      gpio_set_level((gpio_num_t)_cfg.pin_dc, 1);

      spi_bus_config_t buscfg = {};
      buscfg.mosi_io_num = _cfg.pin_mosi;
      buscfg.miso_io_num = -1; // write-only panel
      buscfg.sclk_io_num = _cfg.pin_sclk;
      buscfg.quadwp_io_num = -1;
      buscfg.quadhd_io_num = -1;
      buscfg.max_transfer_sz = MAX_CHUNK;
      buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK;
      if (spi_bus_initialize(_cfg.spi_host, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK)
      {
        return false;
      }

      spi_device_interface_config_t devcfg = {};
      devcfg.mode = _cfg.spi_mode;
      devcfg.clock_speed_hz = _cfg.freq_write;
      devcfg.spics_io_num = -1; // Panel_GC9B72 drives CS itself
      devcfg.queue_size = 1;
      devcfg.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;
      return spi_bus_add_device(_cfg.spi_host, &devcfg, &_spi) == ESP_OK;
    }

    void release(void) override
    {
      if (_spi)
      {
        spi_bus_remove_device(_spi);
        spi_bus_free(_cfg.spi_host);
        _spi = nullptr;
      }
      if (_buf)
      {
        heap_caps_free(_buf);
        _buf = nullptr;
        _bufLen = 0;
      }
    }

    void beginTransaction(void) override {}
    void endTransaction(void) override {}
    void wait(void) override {}
    bool busy(void) const override { return false; }
    void flush(void) override {}

    void initDMA(void) override {}
    void execDMAQueue(void) override {}
    void addDMAQueue(const uint8_t* data, uint32_t length) override
    {
      writeBytes(data, length, true, true);
    }
    uint8_t* getDMABuffer(uint32_t length) override { return _ensure(length); }

    uint32_t getClock(void) const override { return _cfg.freq_write; }
    void setClock(uint32_t freq) override { _cfg.freq_write = freq; }

    bool writeCommand(uint32_t data, uint_fast8_t bit_length) override
    {
      _writeScalar(data, bit_length, false);
      return true;
    }

    void writeData(uint32_t data, uint_fast8_t bit_length) override
    {
      _writeScalar(data, bit_length, true);
    }

    void writeDataRepeat(uint32_t data, uint_fast8_t bit_length, uint32_t count) override
    {
      const uint32_t bytes = bit_length >> 3;
      if (bytes == 0 || count == 0) { return; }

      // Build one buffer holding the repeated pattern, then send it in chunks.
      const uint32_t perChunk = std::max<uint32_t>(1, std::min(count, MAX_CHUNK / bytes));
      uint8_t* buf = _ensure(perChunk * bytes);
      for (uint32_t i = 0; i < perChunk; i++)
      {
        for (uint32_t b = 0; b < bytes; b++)
        {
          buf[i * bytes + b] = (data >> (b * 8)) & 0xFF;
        }
      }

      _setDC(true);
      while (count)
      {
        const uint32_t n = std::min(count, perChunk);
        _transmit(buf, n * bytes);
        count -= n;
      }
    }

    void writePixels(pixelcopy_t* pc, uint32_t length) override
    {
      const uint32_t bytes = pc->dst_bits >> 3;
      if (bytes == 0) { return; }

      const uint32_t perChunk = std::max<uint32_t>(1, MAX_CHUNK / bytes);
      uint8_t* buf = _ensure(std::min(length, perChunk) * bytes);

      _setDC(true);
      while (length)
      {
        const uint32_t n = std::min(length, perChunk);
        pc->fp_copy(buf, 0, n, pc);
        _transmit(buf, n * bytes);
        length -= n;
      }
    }

    void writeBytes(const uint8_t* data, uint32_t length, bool dc, bool use_dma) override
    {
      (void)use_dma;
      _setDC(dc);
      while (length)
      {
        const uint32_t n = std::min(length, MAX_CHUNK);
        // Copy into DMA-capable RAM; callers may hand us flash or stack memory.
        uint8_t* buf = _ensure(n);
        memcpy(buf, data, n);
        _transmit(buf, n);
        data += n;
        length -= n;
      }
    }

    // The panel is wired write-only (no SDO), so reads are stubs.
    void beginRead(void) override {}
    void endRead(void) override {}
    uint32_t readData(uint_fast8_t) override { return 0; }
    bool readBytes(uint8_t*, uint32_t, bool) override { return false; }
    void readPixels(void*, pixelcopy_t*, uint32_t) override {}

  private:
    config_t _cfg;
    spi_device_handle_t _spi = nullptr;
    uint8_t* _buf = nullptr;
    uint32_t _bufLen = 0;

    void _setDC(bool data)
    {
      gpio_set_level((gpio_num_t)_cfg.pin_dc, data ? 1 : 0);
    }

    uint8_t* _ensure(uint32_t length)
    {
      if (length > _bufLen)
      {
        if (_buf) { heap_caps_free(_buf); }
        _buf = (uint8_t*)heap_caps_malloc(length, MALLOC_CAP_DMA);
        _bufLen = _buf ? length : 0;
      }
      return _buf;
    }

    void _transmit(const uint8_t* data, size_t len)
    {
      if (!len || !_spi) { return; }
      spi_transaction_t t = {};
      t.length = len * 8; // bits out
      t.rxlength = 0;     // half duplex, nothing read back
      t.tx_buffer = data;
      spi_device_polling_transmit(_spi, &t);
    }

    // LovyanGFX packs multi-byte values little-endian: the low byte goes first.
    void _writeScalar(uint32_t data, uint_fast8_t bit_length, bool dc)
    {
      const uint32_t bytes = bit_length >> 3;
      if (bytes == 0) { return; }
      uint8_t* buf = _ensure(bytes);
      for (uint32_t b = 0; b < bytes; b++)
      {
        buf[b] = (data >> (b * 8)) & 0xFF;
      }
      _setDC(dc);
      _transmit(buf, bytes);
    }
  };

//----------------------------------------------------------------------------
 }
}
