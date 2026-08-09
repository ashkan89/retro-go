#include <rg_system.h>

#include <stdlib.h>
#include <string.h>

#include "media_image.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_ART"

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
#define MEDIA_USE_ROM_TJPGD 1
#include <esp32s3/rom/tjpgd.h>
#elif defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32)
#define MEDIA_USE_ROM_TJPGD 1
#include <esp32/rom/tjpgd.h>
#else
#define MEDIA_USE_ROM_TJPGD 0
#endif

/* -------------------------------------------------------------------------------------- */
/* Colour helpers (565LE)                                                                   */
/* -------------------------------------------------------------------------------------- */

static inline void unpack565(rg_color_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >> 5) & 0x3F) << 2;
    *b = (c & 0x1F) << 3;
}

static inline rg_color_t pack565(int r, int g, int b)
{
    r = media_clampi(r, 0, 255);
    g = media_clampi(g, 0, 255);
    b = media_clampi(b, 0, 255);
    return (rg_color_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

rg_color_t media_color_blend(rg_color_t a, rg_color_t b, int alpha)
{
    int ar, ag, ab, br, bg, bb;
    unpack565(a, &ar, &ag, &ab);
    unpack565(b, &br, &bg, &bb);
    alpha = media_clampi(alpha, 0, 255);
    return pack565(ar + ((br - ar) * alpha) / 255, ag + ((bg - ag) * alpha) / 255,
                   ab + ((bb - ab) * alpha) / 255);
}

rg_color_t media_color_scale(rg_color_t color, int scale)
{
    int r, g, b;
    unpack565(color, &r, &g, &b);
    return pack565((r * scale) / 255, (g * scale) / 255, (b * scale) / 255);
}

int media_color_luma(rg_color_t color)
{
    int r, g, b;
    unpack565(color, &r, &g, &b);
    return (r * 77 + g * 150 + b * 29) >> 8;
}

/* -------------------------------------------------------------------------------------- */
/* JPEG                                                                                     */
/* -------------------------------------------------------------------------------------- */

#if MEDIA_USE_ROM_TJPGD

// TJpgDec needs a little over 3 KB; give it a round 4 KB from the internal heap because it
// is touched constantly during the decode.
#define TJPGD_POOL_SIZE 4096

typedef struct
{
    const uint8_t *data;
    size_t len;
    size_t pos;
    rg_image_t *out;
    int scale_shift;
} jpeg_ctx_t;

static UINT jpeg_in(JDEC *jd, BYTE *buffer, UINT count)
{
    jpeg_ctx_t *ctx = jd->device;
    size_t remaining = ctx->len > ctx->pos ? ctx->len - ctx->pos : 0;
    if (count > remaining)
        count = (UINT)remaining;

    if (buffer)
        memcpy(buffer, ctx->data + ctx->pos, count);

    ctx->pos += count;
    return count;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = jd->device;
    rg_image_t *img = ctx->out;
    const uint8_t *src = bitmap; // RGB888, JD_FORMAT == 0

    if (!img)
        return 1;

    int block_width = rect->right - rect->left + 1;

    for (int y = rect->top; y <= rect->bottom; ++y)
    {
        if (y < 0 || y >= img->height)
            continue;
        uint16_t *dst = (uint16_t *)img->data + (size_t)y * (img->stride / 2);
        const uint8_t *line = src + (size_t)(y - rect->top) * block_width * 3;

        for (int x = rect->left; x <= rect->right; ++x)
        {
            if (x < 0 || x >= img->width)
                continue;
            const uint8_t *p = line + (size_t)(x - rect->left) * 3;
            dst[x] = (uint16_t)pack565(p[0], p[1], p[2]);
        }
    }

    return 1;
}

static rg_image_t *jpeg_decode(const uint8_t *data, size_t len, int max_dim)
{
    JDEC *jd = calloc(1, sizeof(JDEC));
    uint8_t *pool = rg_alloc(TJPGD_POOL_SIZE, MEM_FAST | MEM_8BIT | MEM_NOPANIC);
    jpeg_ctx_t ctx = {.data = data, .len = len, .pos = 0};

    if (!jd || !pool)
    {
        free(jd);
        free(pool);
        return NULL;
    }

    JRESULT res = jd_prepare(jd, jpeg_in, pool, TJPGD_POOL_SIZE, &ctx);
    if (res != JDR_OK)
    {
        RG_LOGD("jd_prepare failed: %d", res);
        free(jd);
        free(pool);
        return NULL;
    }

    if (jd->width == 0 || jd->height == 0 ||
        (uint64_t)jd->width * jd->height > MEDIA_MAX_ARTWORK_PIXELS)
    {
        free(jd);
        free(pool);
        return NULL;
    }

    // TJpgDec can only descale by powers of two up to 1/8; pick the largest shift that still
    // leaves us at or above the requested size so we never expand a huge image in RAM.
    int shift = 0;
    while (shift < 3 && max_dim > 0 &&
           ((int)(jd->width >> (shift + 1)) >= max_dim || (int)(jd->height >> (shift + 1)) >= max_dim))
        shift++;

    int width = (int)(jd->width >> shift);
    int height = (int)(jd->height >> shift);
    if (width < 1 || height < 1)
    {
        free(jd);
        free(pool);
        return NULL;
    }

    rg_image_t *img = rg_surface_create(width, height, RG_PIXEL_565_LE, MEM_SLOW);
    if (!img)
    {
        free(jd);
        free(pool);
        return NULL;
    }

    ctx.out = img;
    ctx.scale_shift = shift;
    res = jd_decomp(jd, jpeg_out, (BYTE)shift);

    free(jd);
    free(pool);

    if (res != JDR_OK)
    {
        RG_LOGD("jd_decomp failed: %d", res);
        rg_surface_free(img);
        return NULL;
    }

    // Still larger than requested (e.g. 1/8 of a 4000px image): finish with a box resize.
    if (max_dim > 0 && (width > max_dim || height > max_dim))
    {
        int new_w = width, new_h = height;
        if (width >= height)
        {
            new_w = max_dim;
            new_h = media_clampi(height * max_dim / width, 1, max_dim);
        }
        else
        {
            new_h = max_dim;
            new_w = media_clampi(width * max_dim / height, 1, max_dim);
        }
        rg_image_t *scaled = rg_surface_resize(img, new_w, new_h);
        if (scaled)
        {
            rg_surface_free(img);
            img = scaled;
        }
    }

    return img;
}

#else

static rg_image_t *jpeg_decode(const uint8_t *data, size_t len, int max_dim)
{
    (void)data, (void)len, (void)max_dim;
    return NULL; // No JPEG decoder on this platform; PNG covers still work
}

#endif /* MEDIA_USE_ROM_TJPGD */

/* -------------------------------------------------------------------------------------- */
/* Public decoding                                                                          */
/* -------------------------------------------------------------------------------------- */

rg_image_t *media_image_decode(const uint8_t *data, size_t len, int max_dim)
{
    if (!data || len < 16)
        return NULL;

    if (data[0] == 0xFF && data[1] == 0xD8)
        return jpeg_decode(data, len, max_dim);

    if (memcmp(data, "\x89PNG", 4) == 0)
    {
        rg_image_t *img = rg_surface_load_image(data, len, 0);
        if (img && max_dim > 0 && (img->width > max_dim || img->height > max_dim))
        {
            int new_w, new_h;
            if (img->width >= img->height)
            {
                new_w = max_dim;
                new_h = media_clampi(img->height * max_dim / img->width, 1, max_dim);
            }
            else
            {
                new_h = max_dim;
                new_w = media_clampi(img->width * max_dim / img->height, 1, max_dim);
            }
            rg_image_t *scaled = rg_surface_resize(img, new_w, new_h);
            if (scaled)
            {
                rg_surface_free(img);
                img = scaled;
            }
        }
        return img;
    }

    return NULL;
}

rg_image_t *media_image_load_file(const char *path, int max_dim)
{
    if (!path || !*path)
        return NULL;

    rg_stat_t info = rg_storage_stat(path);
    if (!info.exists || info.size < 16 || info.size > MEDIA_MAX_ARTWORK_BYTES)
        return NULL;

    void *data = NULL;
    size_t len = 0;
    if (!rg_storage_read_file(path, &data, &len, 0))
        return NULL;

    rg_image_t *img = media_image_decode(data, len, max_dim);
    free(data);
    return img;
}

/* -------------------------------------------------------------------------------------- */
/* Palette extraction                                                                       */
/* -------------------------------------------------------------------------------------- */

/** Ensure `color` is bright and saturated enough to read as an accent against dark chrome. */
static rg_color_t boost_accent(rg_color_t color)
{
    int r, g, b;
    unpack565(color, &r, &g, &b);

    int max = RG_MAX(r, RG_MAX(g, b));
    int min = RG_MIN(r, RG_MIN(g, b));

    if (max < 96)
    {
        // Too dark to be an accent - lift it
        int lift = 96 - max;
        r += lift, g += lift, b += lift;
        max += lift, min += lift;
    }

    // Push saturation up a little so grey-ish covers still produce a visible tint
    int mid = (max + min) / 2;
    r = mid + ((r - mid) * 3) / 2;
    g = mid + ((g - mid) * 3) / 2;
    b = mid + ((b - mid) * 3) / 2;

    return pack565(r, g, b);
}

media_palette_t media_image_palette(const rg_image_t *image)
{
    media_palette_t palette = {0};

    if (!image || !image->data || image->width < 2 || image->height < 2)
        return palette;

    // 4x4x4 colour cube histogram over a sampled grid. Cheap and good enough to pick an accent.
    static const int BINS = 4;
    uint16_t counts[64] = {0};
    uint32_t sum_r[64] = {0}, sum_g[64] = {0}, sum_b[64] = {0};
    uint32_t total_r = 0, total_g = 0, total_b = 0, samples = 0;

    int step_x = RG_MAX(1, image->width / 32);
    int step_y = RG_MAX(1, image->height / 32);

    for (int y = 0; y < image->height; y += step_y)
    {
        const uint16_t *line = (const uint16_t *)image->data + (size_t)y * (image->stride / 2);
        for (int x = 0; x < image->width; x += step_x)
        {
            int r, g, b;
            unpack565(line[x], &r, &g, &b);

            int bin = ((r * BINS) / 256) * BINS * BINS + ((g * BINS) / 256) * BINS + (b * BINS) / 256;
            bin = media_clampi(bin, 0, 63);
            counts[bin]++;
            sum_r[bin] += r;
            sum_g[bin] += g;
            sum_b[bin] += b;

            total_r += r, total_g += g, total_b += b;
            samples++;
        }
    }

    if (!samples)
        return palette;

    // Rank bins by population weighted by saturation, so a large flat black border does not
    // win over the actual artwork colour.
    int best = -1, second = -1;
    int64_t best_score = -1, second_score = -1;

    for (int i = 0; i < 64; ++i)
    {
        if (!counts[i])
            continue;
        int r = (int)(sum_r[i] / counts[i]);
        int g = (int)(sum_g[i] / counts[i]);
        int b = (int)(sum_b[i] / counts[i]);
        int max = RG_MAX(r, RG_MAX(g, b));
        int min = RG_MIN(r, RG_MIN(g, b));
        int saturation = max - min;
        int64_t score = (int64_t)counts[i] * (16 + saturation);

        if (score > best_score)
        {
            second = best, second_score = best_score;
            best = i, best_score = score;
        }
        else if (score > second_score)
        {
            second = i, second_score = score;
        }
    }

    if (best < 0)
        return palette;

    rg_color_t primary = pack565((int)(sum_r[best] / counts[best]), (int)(sum_g[best] / counts[best]),
                                 (int)(sum_b[best] / counts[best]));
    rg_color_t secondary = primary;
    if (second >= 0 && counts[second])
        secondary = pack565((int)(sum_r[second] / counts[second]), (int)(sum_g[second] / counts[second]),
                            (int)(sum_b[second] / counts[second]));

    rg_color_t average = pack565((int)(total_r / samples), (int)(total_g / samples),
                                 (int)(total_b / samples));

    palette.primary = boost_accent(primary);
    palette.secondary = boost_accent(secondary);
    // The background must never fight the text, so it is always heavily darkened.
    palette.background = media_color_scale(average, 56);
    palette.highlight = media_color_blend(palette.primary, C_WHITE, 96);
    palette.text = media_color_luma(palette.background) > 128 ? C_BLACK : C_WHITE;
    palette.valid = true;

    return palette;
}

media_palette_t media_image_palette_from_hash(uint32_t hash)
{
    // Spread the hue around the wheel; keep saturation and value fixed so every generated
    // palette has the same contrast behaviour as an extracted one.
    int hue = (int)(hash % 360);
    int sector = hue / 60;
    int offset = (hue % 60) * 255 / 60;
    int r = 0, g = 0, b = 0;

    switch (sector)
    {
    case 0: r = 255, g = offset, b = 0; break;
    case 1: r = 255 - offset, g = 255, b = 0; break;
    case 2: r = 0, g = 255, b = offset; break;
    case 3: r = 0, g = 255 - offset, b = 255; break;
    case 4: r = offset, g = 0, b = 255; break;
    default: r = 255, g = 0, b = 255 - offset; break;
    }

    media_palette_t palette = {0};
    palette.primary = boost_accent(pack565((r * 3) / 4, (g * 3) / 4, (b * 3) / 4));
    palette.secondary = media_color_blend(palette.primary, C_WHITE, 64);
    palette.background = media_color_scale(palette.primary, 34);
    palette.highlight = media_color_blend(palette.primary, C_WHITE, 110);
    palette.text = C_WHITE;
    palette.valid = true;
    return palette;
}

/* -------------------------------------------------------------------------------------- */
/* Background generation                                                                    */
/* -------------------------------------------------------------------------------------- */

rg_image_t *media_image_make_background(const rg_image_t *source, int width, int height, int darken)
{
    if (!source || !source->data || width < 8 || height < 8)
        return NULL;

    // A true gaussian blur is far too slow here. Downsampling hard and scaling back up
    // produces the same soft wash for a fraction of the work, and the box pass on the small
    // image removes the blockiness the upscale would otherwise leave behind.
    const int small_w = 24;
    const int small_h = media_clampi(small_w * height / RG_MAX(width, 1), 8, 32);

    rg_image_t *small = rg_surface_resize(source, small_w, small_h);
    if (!small)
        return NULL;

    // Two 3x3 box passes over 24x32 pixels: ~1500 operations, imperceptible.
    uint16_t *pixels = small->data;
    int stride = small->stride / 2;
    uint16_t *temp = malloc((size_t)small_w * small_h * sizeof(uint16_t));

    if (temp)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int y = 0; y < small_h; ++y)
            {
                for (int x = 0; x < small_w; ++x)
                {
                    int sr = 0, sg = 0, sb = 0, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        int yy = media_clampi(y + dy, 0, small_h - 1);
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            int xx = media_clampi(x + dx, 0, small_w - 1);
                            int r, g, b;
                            unpack565(pixels[yy * stride + xx], &r, &g, &b);
                            sr += r, sg += g, sb += b, n++;
                        }
                    }
                    temp[y * small_w + x] = (uint16_t)pack565(sr / n, sg / n, sb / n);
                }
            }
            for (int y = 0; y < small_h; ++y)
                memcpy(pixels + y * stride, temp + y * small_w, (size_t)small_w * sizeof(uint16_t));
        }
        free(temp);
    }

    rg_image_t *background = rg_surface_resize(small, width, height);
    rg_surface_free(small);

    if (!background)
        return NULL;

    // Darken and add a subtle vertical gradient so the bottom controls always stay readable.
    uint16_t *dst = background->data;
    int dst_stride = background->stride / 2;

    for (int y = 0; y < height; ++y)
    {
        int gradient = darken - (darken * y) / (height * 3); // Up to a third darker at the bottom
        for (int x = 0; x < width; ++x)
            dst[y * dst_stride + x] = (uint16_t)media_color_scale(dst[y * dst_stride + x], gradient);
    }

    return background;
}
