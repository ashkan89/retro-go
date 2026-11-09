// Explicit diagnostic builds only. Solid-color fixtures contain no third-party artwork.
#include "../../components/media/media_config.h"
#include "../../components/media/media_image.h"
#include "../../components/media/p4_jpeg.h"
#include "../../components/retro-go/drivers/display/p4_ppa.h"
#include "../../components/retro-go/drivers/display/p4_pixels.h"
#include "fixtures/p4_jpeg_colors.h"

void rg_display_p4_benchmark(const rg_surface_t *source);

static bool p4_check_jpeg(const uint8_t *data, size_t size, uint16_t color, int width, int height)
{
    int64_t start = rg_system_timer();
    rg_image_t *image = p4_jpeg_decode(data, size, 0);
    if (!image || image->width != width || image->height != height)
    {
        rg_surface_free(image);
        RG_LOGE("P4 JPEG hardware decode failed: %dx%d color=%04x", width, height, color);
        return false;
    }
    bool ok = true;
    for (int y = 0; y < image->height; ++y)
    {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)image->data + y * image->stride);
        for (int x = 0; x < image->width; ++x)
        {
            uint16_t pixel = row[x];
            int r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
            ok &= (color == 0xf800 && r >= 28 && g <= 3 && b <= 3) ||
                  (color == 0x07e0 && g >= 60 && r <= 3 && b <= 3) ||
                  (color == 0x001f && b >= 28 && r <= 3 && g <= 3);
        }
    }
    RG_LOGI("P4 JPEG hardware %s: %dx%d color=%04x stride=%d time=%lld us",
            ok ? "PASS" : "FAIL", width, height, color, image->stride,
            (long long)(rg_system_timer() - start));
    rg_surface_free(image);
    return ok;
}

static bool p4_check_scaling(int width, int height, int format)
{
    static const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    bool indexed = (format & RG_PIXEL_PALETTE) != 0;
    bool be = (format & ~RG_PIXEL_PALETTE) == RG_PIXEL_565_BE;
    int stride = (width + 4) * (indexed ? 1 : 2);
    rg_surface_t source = {.width = width, .height = height, .stride = stride, .offset = 4,
                           .format = format};
    source.data = heap_caps_malloc(stride * height + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t palette[256];
    source.palette = palette;
    if (!source.data) return false;
    for (int i = 0; i < 256; ++i)
        palette[i] = be ? __builtin_bswap16(colors[i % 4]) : colors[i % 4];
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            uint8_t *p = (uint8_t *)source.data + 4 + y * stride + x * (indexed ? 1 : 2);
            int color = x * 4 / width;
            if (indexed) *p = color;
            else memcpy(p, &palette[color], 2);
        }
    int w = RG_SCREEN_WIDTH, h = RG_SCREEN_HEIGHT;
    p4_fit_viewport(width, height, w, h, true, &w, &h);
    int64_t start = rg_system_timer();
    const rg_surface_t *scaled = NULL;
    for (int i = 0; i < 12; ++i)
        scaled = p4_scale_frame(&source, w, h);
    int64_t elapsed = rg_system_timer() - start;
    bool ok = scaled != NULL;
    bool interpolation = false;
    if (scaled)
    {
        const uint16_t *row = (const uint16_t *)scaled->data + (h / 2) * w;
        for (int c = 0; c < 4; ++c)
            ok &= row[(c * 2 + 1) * w / 8] == colors[c];
        for (int x = 2; x < w - 2; ++x)
            interpolation |= row[x] != colors[0] && row[x] != colors[1] &&
                             row[x] != colors[2] && row[x] != colors[3];
        // Integer upscales also interpolate the edges of these color bands.
        ok &= interpolation;
    }
    RG_LOGI("P4 scaler %s: %dx%d format=%x => %dx%d avg=%lld us palette=%d bilinear=%d",
            ok ? "PASS" : "FAIL", width, height, format, w, h,
            (long long)(elapsed / 12), indexed, interpolation);
    if ((width == 256 && height == 240 && format == RG_PIXEL_PAL565_BE) ||
        (width == 240 && format == RG_PIXEL_565_LE))
        rg_display_p4_benchmark(&source);
    free(source.data);
    return ok;
}

__attribute__((noinline)) static void p4_scalar_pixels(uint16_t *dest, const uint16_t *source, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i)
        dest[i] = __builtin_bswap16(source[i]);
}

static bool p4_check_pixels(void)
{
    uint16_t *source = heap_caps_aligned_alloc(64, 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *dest = heap_caps_aligned_alloc(64, 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!source || !dest) { free(source); free(dest); return false; }
    for (int i = 0; i < 512; ++i) source[i] = (i * 317) ^ 0x1234;
    bool ok = true;
    for (int a = 0; a < 8; ++a)
        for (int b = 0; b < 8; ++b)
            for (int n = 0; n <= 480; n += 15)
            {
                memset(dest, 0xa5, 1024);
                p4_copy_rgb565_be(dest + b, source + a, n);
                for (int i = 0; i < n; ++i)
                    ok &= dest[b + i] == __builtin_bswap16(source[a + i]);
                ok &= dest[b + n] == 0xa5a5;
                if (b) ok &= dest[b - 1] == 0xa5a5;
            }
    int64_t start = rg_system_timer();
    for (int i = 0; i < 320; ++i) p4_copy_rgb565_be(dest, source, 480);
    RG_LOGI("P4 PIE pixels %s: alignments/tails/guards; 480x320 conversion=%lld us", ok ? "PASS" : "FAIL",
            (long long)(rg_system_timer() - start));
    start = rg_system_timer();
    for (int i = 0; i < 320; ++i) p4_scalar_pixels(dest, source, 480);
    RG_LOGI("P4 scalar pixels: 480x320 conversion=%lld us", (long long)(rg_system_timer() - start));
    free(source); free(dest);
    return ok;
}

static void p4_hardware_selftest(void)
{
    bool ok = true;
    ok &= p4_check_jpeg(p4_jpeg_red_420, sizeof(p4_jpeg_red_420), 0xf800, 128, 128);
    ok &= p4_check_jpeg(p4_jpeg_green_422, sizeof(p4_jpeg_green_422), 0x07e0, 128, 128);
    ok &= p4_check_jpeg(p4_jpeg_blue_444, sizeof(p4_jpeg_blue_444), 0x001f, 128, 128);
    ok &= p4_check_jpeg(p4_jpeg_padded_420, sizeof(p4_jpeg_padded_420), 0xf800, 128, 120);
    rg_image_t *rejected = p4_jpeg_decode(p4_jpeg_odd_420, sizeof(p4_jpeg_odd_420), 0);
    ok &= rejected == NULL;
    rg_surface_free(rejected);
    rg_image_t *fallback = media_image_decode(p4_jpeg_odd_420, sizeof(p4_jpeg_odd_420), 0);
    ok &= fallback && fallback->width == 31 && fallback->height == 17;
    rg_surface_free(fallback);
    ok &= p4_check_pixels();
    const int formats[] = {RG_PIXEL_565_LE, RG_PIXEL_565_BE, RG_PIXEL_PAL565_LE, RG_PIXEL_PAL565_BE};
    const int sizes[][2] = {{256, 240}, {240, 160}, {256, 192}, {160, 144}, {320, 224}, {320, 241}};
    for (int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        for (int f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f)
            ok &= p4_check_scaling(sizes[i][0], sizes[i][1], formats[f]);
    p4_scaler_deinit();
    rg_surface_t *source = rg_surface_create(16, 16, RG_PIXEL_565_LE, MEM_SLOW | MEM_NOPANIC);
    if (source)
    {
        for (int i = 0; i < 256; ++i) ((uint16_t *)source->data)[i] = 0xf800;
        const rg_surface_t *scaled = p4_scale_frame(source, 24, 24);
        ok &= scaled != NULL;
        if (scaled)
            for (int i = 0; i < 24 * 24; ++i)
                ok &= ((uint16_t *)scaled->data)[i] == 0xf800;
        p4_scaler_deinit();
        rg_surface_free(source);
    }
    else ok = false;
    if (ok)
        RG_LOGI("P4 HARDWARE SELFTEST PASS: JPEG 420/422/444, RGB565, MCU padding, software fallback, PPA PSRAM readback");
    else
        RG_LOGE("P4 HARDWARE SELFTEST FAIL; returning to launcher without a reboot loop");
}

