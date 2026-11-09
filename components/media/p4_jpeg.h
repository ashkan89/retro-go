// Included only on P4. Each decode owns its engine and DMA output, so concurrent
// artwork requests cannot overwrite one another's surfaces.
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"

static rg_image_t *p4_jpeg_decode(const uint8_t *data, size_t len, int max_dim)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(data, len, &info) != ESP_OK || !info.width || !info.height ||
        (uint64_t)info.width * info.height > MEDIA_MAX_ARTWORK_PIXELS)
        return NULL;
    // Keep small/unaligned thumbnails on the software path. Tiny padded images
    // timed out on rev 1.3; the driver's area-only check is not sufficient.
    if (info.width < 64 || info.height < 64 || info.width % 8 || info.height % 8)
        return NULL;

    unsigned align_x, align_y;
    switch (info.sample_method)
    {
        case JPEG_DOWN_SAMPLING_YUV444: align_x = align_y = 8; break;
        case JPEG_DOWN_SAMPLING_YUV422: align_x = 16; align_y = 8; break;
        case JPEG_DOWN_SAMPLING_YUV420: align_x = align_y = 16; break;
        default: return NULL; // Grayscale and unsupported modes use software.
    }
    size_t stride = ((info.width + align_x - 1) & ~(size_t)(align_x - 1)) * 2;
    size_t rows = (info.height + align_y - 1) & ~(size_t)(align_y - 1);
    size_t size = stride * rows;
    // Full-size DMA decode is useful for ordinary covers. Very large artwork
    // uses the software decoder's downscaling instead of monopolizing PSRAM.
    if (size > 4 * 1024 * 1024 || len > MEDIA_MAX_ARTWORK_BYTES ||
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < size + len + 2 * 1024 * 1024)
        return NULL;

    const jpeg_decode_memory_alloc_cfg_t memory = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t capacity = 0;
    uint8_t *pixels = jpeg_alloc_decoder_mem(size, &memory, &capacity);
    if (!pixels)
        return NULL;
    // IDF 5.5 feeds the entropy stream directly to 2D-DMA. Stage it in
    // allocated PSRAM so embedded/flash-mapped inputs work as well as files.
    // In particular, XiP constants are not ordinary heap DMA buffers.
    const jpeg_decode_memory_alloc_cfg_t input_memory = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    size_t input_capacity = 0;
    uint8_t *input = jpeg_alloc_decoder_mem(len, &input_memory, &input_capacity);
    if (!input)
    {
        free(pixels);
        return NULL;
    }
    memcpy(input, data, len);
    const jpeg_decode_engine_cfg_t engine_config = {.timeout_ms = 100};
    jpeg_decoder_handle_t engine = NULL;
    const jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR, // Little-endian RGB565
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t written = 0;
    esp_err_t err = jpeg_new_decoder_engine(&engine_config, &engine);
    if (err == ESP_OK)
    {
        err = jpeg_decoder_process(engine, &config, input, len, pixels, capacity, &written);
        jpeg_del_decoder_engine(engine);
    }
    free(input);
    if (err != ESP_OK || written != size)
    {
        free(pixels);
        return NULL;
    }
    rg_image_t *image = calloc(1, sizeof(*image));
    if (!image)
    {
        free(pixels);
        return NULL;
    }
    *image = (rg_image_t){.width = info.width, .height = info.height,
        .stride = stride, .format = RG_PIXEL_565_LE, .data = pixels, .free_data = true};
    // Keep the MCU padding in the stride, but expose only the original pixels.
    if (max_dim > 0 && (image->width > max_dim || image->height > max_dim))
    {
        int width = image->width >= image->height ? max_dim : RG_MAX(1, image->width * max_dim / image->height);
        int height = image->height >= image->width ? max_dim : RG_MAX(1, image->height * max_dim / image->width);
        rg_image_t *scaled = rg_surface_resize(image, width, height);
        rg_surface_free(image);
        image = scaled;
    }
    return image;
}
