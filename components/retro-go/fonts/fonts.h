#include "../rg_gui.h"

/**
 * This file can be edited to add fonts to retro-go.
 * To create new fonts you can use font_converter.py located in the tools folder.
 *
 * The Sans fonts carry the Arabic block and the Arabic presentation forms on top of Latin-1, which
 * is what rg_text_shape() needs in order to draw Persian and Arabic joined and right to left. The
 * other fonts are Latin-only, so pick a Sans one for those languages.
 */

extern const rg_font_t font_basic8x8;
extern const rg_font_t font_DejaVu12;
extern const rg_font_t font_DejaVu15;
extern const rg_font_t font_VeraBold11;
extern const rg_font_t font_VeraBold14;
extern const rg_font_t font_Sans12;
extern const rg_font_t font_Sans15;

enum {
    RG_FONT_BASIC_8,
    RG_FONT_BASIC_12,
    RG_FONT_BASIC_16,
    RG_FONT_DEJAVU_12,
    RG_FONT_DEJAVU_15,
    RG_FONT_VERA_11,
    RG_FONT_VERA_14,
    RG_FONT_SANS_12,
    RG_FONT_SANS_15,
    RG_FONT_MAX,
};

static const rg_font_t *fonts[RG_FONT_MAX] = {
    &font_basic8x8,
    &font_basic8x8,
    &font_basic8x8,
    &font_DejaVu12,
    &font_DejaVu15,
    &font_VeraBold11,
    &font_VeraBold14,
    &font_Sans12,
    &font_Sans15,
};
