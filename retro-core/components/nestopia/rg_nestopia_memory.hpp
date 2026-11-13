#pragma once
#include <cstdlib>
#include <new>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace RetroGo
{
// Machine state and the PPU working screen prefer internal SRAM.
// The large callback table and cartridge allocations use normal new.
// Fall back gracefully on boards with less SRAM or a fragmented internal heap.
struct HotAllocator
{
    static void *operator new(std::size_t size)
    {
        void *memory = nullptr;
#ifdef ESP_PLATFORM
        memory = heap_caps_aligned_alloc(16, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory)
            memory = heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT);
#else
        if (!memory)
            memory = std::malloc(size);
#endif
        if (!memory)
            throw std::bad_alloc();
        return memory;
    }

    static void operator delete(void *memory) noexcept
    {
        std::free(memory);
    }
};
}
