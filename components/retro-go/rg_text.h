#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Turn logical text into the sequence of glyphs to draw, left to right.
 *
 * The GUI renderer draws one glyph after another from left to right, which is all a Latin script
 * needs. Arabic and Persian need two things on top of that: each letter has up to four shapes
 * depending on its neighbours, and the text runs right to left with numbers and Latin words inside
 * it still running left to right. Both are resolved here, so the renderer stays a simple loop.
 *
 * The returned string is UTF-8 containing Arabic presentation forms (U+FB50.. and U+FE70..), in
 * display order. It points at a static buffer that is valid until the next call, and text that
 * needs no shaping is returned unchanged (no copy, no cost).
 */
const char *rg_text_shape(const char *text);

/* True if the string contains any right-to-left character, which a caller may want in order to
 * align it to the right. */
bool rg_text_is_rtl(const char *text);
