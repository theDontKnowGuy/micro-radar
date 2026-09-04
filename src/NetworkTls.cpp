#include "NetworkTls.h"

#include <esp_heap_caps.h>
#include <freertos/task.h>

namespace NetworkTls {
namespace {

SemaphoreHandle_t tlsMutex = nullptr;
StaticSemaphore_t tlsMutexStorage;

}  // namespace

bool Begin()
{
    if (tlsMutex != nullptr)
        return true;

    // Recursive because an aircraft fetch owns the gate across response parsing
    // and may call HttpRequestManager, which takes the same gate around the
    // socket itself.
    tlsMutex = xSemaphoreCreateRecursiveMutexStatic(&tlsMutexStorage);
    return tlsMutex != nullptr;
}

Guard::Guard()
{
    if (tlsMutex != nullptr)
        locked = xSemaphoreTakeRecursive(tlsMutex, portMAX_DELAY) == pdTRUE;
}

Guard::~Guard()
{
    if (locked)
        xSemaphoreGiveRecursive(tlsMutex);
}

void LogHeap(const char* phase, const char* operation, const char* url)
{
    constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const char* task = pcTaskGetName(nullptr);
    Serial.printf("[HTTP] %s %s %s [task=%s]: free %u, largest %u, minimum %u\n",
                  phase,
                  operation,
                  url,
                  task != nullptr ? task : "unknown",
                  heap_caps_get_free_size(caps),
                  heap_caps_get_largest_free_block(caps),
                  heap_caps_get_minimum_free_size(caps));
}

}  // namespace NetworkTls
