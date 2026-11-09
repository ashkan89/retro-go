// P4 scaler fast path. Preserve the software renderer for unsupported geometry
// and nearest-neighbour filtering; SRM uses bilinear filtering and 1/16 steps.
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "p4_scale_geometry.h"

static ppa_client_handle_t ppa_scaler;
static rg_surface_t ppa_surface;
static size_t ppa_capacity;
static bool ppa_unavailable;
static void *ppa_input;
static size_t ppa_input_capacity;

static bool p4_scaler_buffer(void **buffer, size_t *capacity, size_t size)
{
    size = (size + 63) & ~(size_t)63;
    // Bound retained scratch space, particularly when a custom zoom is used.
    if (size > 4 * 1024 * 1024)
        return false;
    if (*capacity < size)
    {
        free(*buffer);
        *buffer = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        *capacity = *buffer ? size : 0;
    }
    return *buffer != NULL;
}

static const rg_surface_t *p4_scale_frame(const rg_surface_t *source, int width, int height)
{
    int scale_x = p4_scale_factor(source->width, width);
    int scale_y = p4_scale_factor(source->height, height);
    bool indexed = source->format == RG_PIXEL_PAL565_BE || source->format == RG_PIXEL_PAL565_LE;
    if (ppa_unavailable || !source->data || !scale_x || !scale_y || source->offset < 0 ||
        (!indexed && source->format != RG_PIXEL_565_LE && source->format != RG_PIXEL_565_BE) ||
        source->stride < source->width * (indexed ? 1 : 2) ||
        (!indexed && (source->stride % 2 || source->offset % 2)) ||
        (indexed && !source->palette) || (width == source->width && height == source->height))
        return NULL;
    if (!ppa_scaler)
    {
        const ppa_client_config_t config = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
        if (ppa_register_client(&config, &ppa_scaler) != ESP_OK)
        {
            ppa_unavailable = true;
            return NULL;
        }
        RG_LOGI("P4 PPA hardware scaler ready");
    }
    if (!p4_scaler_buffer(&ppa_surface.data, &ppa_capacity, (size_t)width * height * 2))
        return NULL;
    const void *input = (const uint8_t *)source->data + source->offset;
    int input_width = source->stride / 2;
    bool byte_swap = source->format == RG_PIXEL_565_BE;
    if (indexed)
    {
        if (!p4_scaler_buffer(&ppa_input, &ppa_input_capacity,
                              (size_t)source->width * source->height * 2))
            return NULL;
        // Expand at SOURCE resolution. SRM then performs interpolation and
        // scaling without the CPU palette lookup/filter work per output pixel.
        // Keep palette byte order and let PPA swap BE input in hardware.
        for (int y = 0; y < source->height; ++y)
        {
            const uint8_t *row = (const uint8_t *)input + y * source->stride;
            uint16_t *dest = (uint16_t *)ppa_input + y * source->width;
            for (int x = 0; x < source->width; ++x)
                dest[x] = source->palette[row[x]];
        }
        input = ppa_input;
        input_width = source->width;
        byte_swap = source->format == RG_PIXEL_PAL565_BE;
    }
    const ppa_srm_oper_config_t operation = {
        .in = {.buffer = input,
               .pic_w = input_width, .pic_h = source->height,
               .block_w = source->width, .block_h = source->height,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = ppa_surface.data, .buffer_size = ppa_capacity,
                .pic_w = width, .pic_h = height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .scale_x = scale_x / 16.0f, .scale_y = scale_y / 16.0f,
        .byte_swap = byte_swap,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(ppa_scaler, &operation) != ESP_OK)
        return NULL;
    ppa_surface.width = width;
    ppa_surface.height = height;
    ppa_surface.stride = width * 2;
    ppa_surface.format = RG_PIXEL_565_LE;
    return &ppa_surface;
}

static void p4_scaler_deinit(void)
{
    if (ppa_scaler)
        ppa_unregister_client(ppa_scaler);
    ppa_scaler = NULL;
    free(ppa_surface.data);
    free(ppa_input);
    ppa_input = NULL;
    ppa_input_capacity = 0;
    ppa_surface = (rg_surface_t){0};
    ppa_capacity = 0;
    ppa_unavailable = false;
}
