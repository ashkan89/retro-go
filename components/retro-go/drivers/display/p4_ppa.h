// P4 scaler fast path. Preserve the software renderer for unsupported geometry
// and nearest-neighbour filtering; SRM uses bilinear filtering and 1/16 steps.
#include "driver/ppa.h"
#include "esp_heap_caps.h"

static ppa_client_handle_t ppa_scaler;
static rg_surface_t ppa_surface;
static size_t ppa_capacity;
static bool ppa_unavailable;

static const rg_surface_t *p4_scale_frame(const rg_surface_t *source, int width, int height)
{
    if (ppa_unavailable || (source->format != RG_PIXEL_565_LE && source->format != RG_PIXEL_565_BE) ||
        width <= 0 || height <= 0 || source->stride % 2 ||
        (width == source->width && height == source->height) ||
        (width * 16) % source->width || (height * 16) % source->height)
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
    size_t size = (width * height * 2 + 63) & ~(size_t)63;
    if (ppa_capacity < size)
    {
        free(ppa_surface.data);
        ppa_surface.data = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ppa_capacity = ppa_surface.data ? size : 0;
        if (!ppa_surface.data)
            return NULL;
    }
    const ppa_srm_oper_config_t operation = {
        .in = {.buffer = (const uint8_t *)source->data + source->offset,
               .pic_w = source->stride / 2, .pic_h = source->height,
               .block_w = source->width, .block_h = source->height,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = ppa_surface.data, .buffer_size = ppa_capacity,
                .pic_w = width, .pic_h = height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .scale_x = (float)width / source->width, .scale_y = (float)height / source->height,
        .byte_swap = source->format == RG_PIXEL_565_BE,
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
    ppa_surface = (rg_surface_t){0};
    ppa_capacity = 0;
}
