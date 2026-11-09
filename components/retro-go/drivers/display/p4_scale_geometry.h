#pragma once
#include <stdbool.h>

// SRM truncates its scale factors to 1/16 and floors the output dimensions.
// Pick a representable viewport once, rather than rejecting every frame or
// stretching an incompletely written DMA buffer to the requested dimensions.
static inline int p4_scale_factor(int source, int output)
{
    if (source <= 0 || output <= 0 || source > 2048 || output > 2048)
        return 0;
    int factor = (output * 16 + source - 1) / source;
    return factor > 0 && factor < 4096 && source * factor / 16 == output ? factor : 0;
}

static inline void p4_fit_viewport(int source_width, int source_height,
                                  int screen_width, int screen_height, bool preserve_aspect,
                                  int *width, int *height)
{
    int x = screen_width * 16 / source_width;
    int y = screen_height * 16 / source_height;
    if (preserve_aspect)
        x = y = x < y ? x : y;
    if (x > 0 && y > 0)
    {
        *width = source_width * x / 16;
        *height = source_height * y / 16;
    }
}
