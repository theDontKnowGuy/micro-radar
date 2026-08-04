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
#include <esp_system.h>

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

  class Bus_GC9B72_SPI : public IBus
  {
    static constexpr uint32_t MAX_CHUNK = 4096;

    // Bulk pixel writes are queued asynchronously across two DMA buffers so the
    // CPU can convert the next chunk while the current one is still on the wire.
    // Measured on a 360x360 frame at 80MHz: the transfer itself is ~26ms but a
    // convert-then-block loop took ~50ms, because the pixel format conversion
    // was serialised behind every transfer.
    static constexpr int SLOTS = 2;

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
      devcfg.queue_size = SLOTS + 1; // room to keep SLOTS transfers in flight
      devcfg.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;
      if (spi_bus_add_device(_cfg.spi_host, &devcfg, &_spi) != ESP_OK)
      {
        return false;
      }

      // Reserve the DMA buffers now, while the heap is still empty. They are
      // small and fixed size, and allocating them lazily meant competing with
      // WiFi/TLS for memory in the middle of rendering.
      for (int s = 0; s < SLOTS; s++)
      {
        if (!_slotBuf(s)) { return false; }
      }
      if (!_ensure(MAX_CHUNK)) { return false; }

      // Resetting the chip with DMA transfers still in flight corrupts memory
      // during the next boot. The main loop already waits for the bus before a
      // settings restart, but this covers the paths we do not control -- OTA,
      // WiFiManager reboots, watchdog resets.
      _shutdownTarget = this;
      esp_register_shutdown_handler(_onShutdown);
      return true;
    }

    void release(void) override
    {
      if (_spi)
      {
        _drain();
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
      for (int s = 0; s < SLOTS; s++)
      {
        if (_chunk[s]) { heap_caps_free(_chunk[s]); _chunk[s] = nullptr; }
      }
    }

    void beginTransaction(void) override {}

    // Panel_LCD::end_transaction() calls wait() before raising CS, so draining
    // here is what guarantees CS never drops mid-transfer.
    void endTransaction(void) override { _drain(); }
    void wait(void) override { _drain(); }
    bool busy(void) const override { return _pending > 0; }
    void flush(void) override { _drain(); }

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

      // Uses its own buffer and stays synchronous. This is not in the render
      // path -- pushSprite goes through writePixels -- so it is not worth
      // sharing the pipelined slot buffers and the ownership rules they carry.
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

      _setDC(true);
      while (length)
      {
        const uint32_t n = std::min(length, perChunk);
        // Convert into a slot that is already free, so this CPU work overlaps
        // the transfer still on the wire.
        uint8_t* buf = _nextFreeSlot();
        pc->fp_copy(buf, 0, n, pc);
        _queue(_slot, buf, n * bytes);
        _slot = (_slot + 1) % SLOTS;
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
        // Copy into DMA-capable RAM; callers may hand us flash or stack memory,
        // and the copy also frees them to reuse their buffer immediately even
        // though the transfer is still in flight.
        uint8_t* buf = _nextFreeSlot();
        memcpy(buf, data, n);
        _queue(_slot, buf, n);
        _slot = (_slot + 1) % SLOTS;
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

    uint8_t* _chunk[SLOTS] = {};
    spi_transaction_t _trans[SLOTS] = {};
    int _pending = 0;
    // Persists across calls: a bulk write can return with transfers still in
    // flight, so the next one must not assume slot 0 is free.
    int _slot = 0;

    // Make _chunk[_slot] safe to overwrite, then hand it back. Transfers are
    // reaped in submission order, so bounding the queue below SLOTS guarantees
    // the slot we are about to reuse has already completed.
    uint8_t* _nextFreeSlot(void)
    {
      if (_pending >= SLOTS) { _reap(); }
      return _slotBuf(_slot);
    }

    uint8_t* _slotBuf(int slot)
    {
      if (!_chunk[slot])
      {
        _chunk[slot] = (uint8_t*)heap_caps_malloc(MAX_CHUNK, MALLOC_CAP_DMA);
      }
      return _chunk[slot];
    }

    // Queue a transfer without waiting for it. The buffer and the transaction
    // struct must both stay untouched until the matching _reap().
    void _queue(int slot, const uint8_t* data, size_t len)
    {
      if (!len || !_spi) { return; }
      spi_transaction_t& t = _trans[slot];
      memset(&t, 0, sizeof t);
      t.length = len * 8;
      t.rxlength = 0;
      t.tx_buffer = data;
      if (spi_device_queue_trans(_spi, &t, portMAX_DELAY) == ESP_OK)
      {
        _pending++;
      }
    }

    void _reap(void)
    {
      if (_pending <= 0) { return; }
      spi_transaction_t* done = nullptr;
      if (spi_device_get_trans_result(_spi, &done, portMAX_DELAY) == ESP_OK)
      {
        _pending--;
      }
    }

    void _drain(void)
    {
      while (_pending > 0) { _reap(); }
    }

    // esp_register_shutdown_handler() takes a plain function pointer, so the
    // active instance is tracked here. Shutdown handlers run before interrupts
    // are disabled, so the DMA completion interrupts this waits on still fire.
    static inline Bus_GC9B72_SPI* _shutdownTarget = nullptr;

    static void _onShutdown(void)
    {
      if (_shutdownTarget) { _shutdownTarget->_drain(); }
    }

    // Any queued transfer was issued for the CURRENT state of DC, so the queue
    // has to drain before DC moves. Without this, pixel bytes still on the wire
    // get clocked out after DC has flipped to command and the panel decodes
    // them as commands.
    void _setDC(bool data)
    {
      _drain();
      gpio_set_level((gpio_num_t)_cfg.pin_dc, data ? 1 : 0);
    }

    // Grows without ever dropping the existing buffer. The previous version
    // freed first and then allocated, so a failed allocation under memory
    // pressure left _buf null and handed callers a null pointer to draw into --
    // turning a transient low-heap moment into corrupted output.
    uint8_t* _ensure(uint32_t length)
    {
      if (length <= _bufLen) { return _buf; }

      uint8_t* grown = (uint8_t*)heap_caps_malloc(length, MALLOC_CAP_DMA);
      if (!grown) { return _buf; } // keep what we have rather than losing it

      if (_buf) { heap_caps_free(_buf); }
      _buf = grown;
      _bufLen = length;
      return _buf;
    }

    // Synchronous send, used for commands and other small writes. Any queued
    // bulk transfers must finish first, both to keep command ordering correct
    // and because polling and queued transactions cannot be interleaved.
    void _transmit(const uint8_t* data, size_t len)
    {
      if (!len || !_spi) { return; }
      _drain();
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
