// Explicit diagnostic builds only. Solid-color fixtures contain no third-party artwork.
#include "../../components/media/media_config.h"
#include "../../components/media/media_image.h"
#include "../../components/media/p4_jpeg.h"
#include "../../components/retro-go/drivers/display/p4_ppa.h"
#include "fixtures/p4_jpeg_colors.h"

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

