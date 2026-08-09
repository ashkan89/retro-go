/**
 * Retro-Go media player - image decoding and colour analysis.
 *
 * JPEG goes through TJpgDec, which lives in the ESP32-S3 ROM and therefore costs no flash.
 * It also descales by 1/2, 1/4 or 1/8 while decoding, so a 3000x3000 cover is never fully
 * expanded in RAM. PNG reuses retro-go's lodepng path.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

#include <rg_surface.h>

/** A small palette derived from an image, used to tint the player UI. */
typedef struct
{
    rg_color_t primary;     // Most saturated dominant colour
    rg_color_t secondary;
    rg_color_t background;  // Darkened average, always safe behind white text
    rg_color_t highlight;
    rg_color_t text;        // Either near-white or near-black, whichever contrasts
    bool valid;
} media_palette_t;

/**
 * Decode `data` (JPEG or PNG) to a 565LE surface whose longest edge is at most `max_dim`.
 * Returns NULL on any failure, including malformed input. Never aborts.
 */
rg_image_t *media_image_decode(const uint8_t *data, size_t len, int max_dim);

/** Same, but reads the file first. */
rg_image_t *media_image_load_file(const char *path, int max_dim);

/** Derive a palette. Cheap: it samples a grid rather than every pixel. */
media_palette_t media_image_palette(const rg_image_t *image);

/** Palette used when a track has no artwork, derived from a string hash. */
media_palette_t media_image_palette_from_hash(uint32_t hash);

/**
 * Produce a darkened, blurred version of `source` at exactly width x height, suitable for
 * use as a full-screen background. Returns NULL if memory is short (callers fall back to a
 * flat colour). `darken` is 0..255 where 255 keeps the original brightness.
 */
rg_image_t *media_image_make_background(const rg_image_t *source, int width, int height, int darken);

/** Blend two 565LE colours. `alpha` is 0..255 (0 = a, 255 = b). */
rg_color_t media_color_blend(rg_color_t a, rg_color_t b, int alpha);

/** Scale a 565LE colour's brightness. `scale` is 0..255 (255 = unchanged). */
rg_color_t media_color_scale(rg_color_t color, int scale);

/** Perceptual luminance 0..255 of a 565LE colour. */
int media_color_luma(rg_color_t color);
