#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// One admission gate for every outbound TLS session in the firmware. The
// application clients take it directly; Diagnostics wraps the ESP Insights
// HTTPS transport's synchronous data_send callback with the same guard.
namespace NetworkTls {

// Create the statically backed mutex before any component can start a
// background network task.
bool Begin();

class Guard
{
public:
    Guard();
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    bool locked = false;
};

// Reports the internal 8-bit heap, which is the heap an ESP32-S3 TLS
// handshake needs. "minimum" is the low-water mark since boot.
void LogHeap(const char* phase, const char* operation, const char* url);

// Put one at the start of a caller that parses a response. Its final reading
// happens after that caller's local HttpResult and JsonDocument are destroyed.
class HeapScope
{
public:
    HeapScope(const char* operation, const char* url)
        : operation(operation), url(url) {}
    ~HeapScope() { LogHeap("Caller done", operation, url); }

    HeapScope(const HeapScope&) = delete;
    HeapScope& operator=(const HeapScope&) = delete;

private:
    const char* operation;
    const char* url;
};

}  // namespace NetworkTls
