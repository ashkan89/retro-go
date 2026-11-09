#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void p4_swap_pixels(uint16_t *dest, const uint16_t *source, size_t pixels);

static inline void p4_copy_rgb565_be(uint16_t *dest, const uint16_t *source, size_t pixels)
{
    // The assembly explicitly enables misaligned vector accesses. Without
    // this, PIE silently rounds addresses down and corrupts odd-width rows.
    if (pixels >= 8)
    {
        size_t count = pixels & ~(size_t)7;
        p4_swap_pixels(dest, source, count);
        source += count;
        dest += count;
        pixels -= count;
    }
    while (pixels--)
    {
        uint16_t pixel;
        memcpy(&pixel, source++, sizeof(pixel));
        *dest++ = (pixel << 8) | (pixel >> 8);
    }
}
