/**
 * Retro-Go media player - small shared helpers.
 *
 * Text handling here is deliberately UTF-8 aware: metadata routinely contains Persian,
 * Arabic, Japanese and European text, and truncating on a byte boundary would corrupt it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

/* -------------------------------------------------------------------------------------- */
/* Strings                                                                                  */
/* -------------------------------------------------------------------------------------- */

/** Copy at most dst_size-1 bytes, never splitting a UTF-8 sequence. Always NUL terminates. */
size_t media_utf8_copy(char *dst, size_t dst_size, const char *src);

/** Byte length of the longest prefix of `str` that is <= max_bytes and ends on a codepoint. */
size_t media_utf8_clip(const char *str, size_t max_bytes);

/** Validate/repair in place: replaces malformed bytes with U+FFFD. Returns new length. */
size_t media_utf8_sanitize(char *str, size_t size);

/** Convert Latin-1 (ID3v1 / ID3v2 ISO-8859-1) to UTF-8. Returns bytes written incl. NUL. */
size_t media_latin1_to_utf8(char *dst, size_t dst_size, const uint8_t *src, size_t src_len);

/** Convert UTF-16 (with or without BOM) to UTF-8. `be` selects the default endianness. */
size_t media_utf16_to_utf8(char *dst, size_t dst_size, const uint8_t *src, size_t src_len, bool be);

/** Trim ASCII whitespace and control characters from both ends, in place. */
char *media_str_trim(char *str);

/** Case-insensitive "natural" compare: "2 - Song" sorts before "10 - Song". */
int media_strnatcasecmp(const char *a, const char *b);

/** Case-insensitive substring search (ASCII folding only). */
bool media_str_contains_ci(const char *haystack, const char *needle);

/* -------------------------------------------------------------------------------------- */
/* Paths and file types                                                                     */
/* -------------------------------------------------------------------------------------- */

/** Single place that decides what a media file is. Case-insensitive. */
media_codec_t media_codec_from_path(const char *path);

/**
 * Codec implied by an HTTP Content-Type. A stream URL usually has no extension and starts
 * mid-file, so the server's own answer is the only reliable identification.
 * Any parameters after a ';' are ignored.
 */
media_codec_t media_codec_from_mime(const char *mime);

/** True for the playlist types stations hand out instead of a stream (m3u, m3u8, pls). */
bool media_mime_is_playlist(const char *mime);
bool media_path_is_audio(const char *path);
bool media_path_is_playlist(const char *path);
bool media_path_is_image(const char *path);

/** True when `name` is a directory/file we never index (dotfiles, our own cache). */
bool media_path_is_hidden(const char *name);

/**
 * Resolve `ref` (which may be relative, may use backslashes) against `base_dir` into `out`.
 * Rejects results that escape `root`. Returns false if the result would not fit or escapes.
 */
bool media_path_resolve(char *out, size_t out_size, const char *base_dir, const char *ref,
                        const char *root);

/** Join two path components with exactly one separator. Returns false on overflow. */
bool media_path_join(char *out, size_t out_size, const char *dir, const char *name);

/** Copy the basename without its extension into `out`. */
void media_path_stem(char *out, size_t out_size, const char *path);

/** Replace the extension of `path` (e.g. "lrc"), writing into `out`. */
bool media_path_swap_ext(char *out, size_t out_size, const char *path, const char *ext);

/* -------------------------------------------------------------------------------------- */
/* Formatting                                                                               */
/* -------------------------------------------------------------------------------------- */

/** "M:SS" or "H:MM:SS". Always NUL terminated. */
void media_format_time(char *out, size_t out_size, uint32_t ms);

/** "+00:20" style relative offset used by the seek overlay. */
void media_format_delta(char *out, size_t out_size, int32_t delta_ms);

void media_format_size(char *out, size_t out_size, uint64_t bytes);

/* -------------------------------------------------------------------------------------- */
/* Fixed-point animation helpers (0..65536 fixed 16.16 progress)                             */
/* -------------------------------------------------------------------------------------- */

typedef struct
{
    int64_t start_us;
    uint32_t duration_us;
    int32_t from;
    int32_t to;
    bool active;
} media_anim_t;

void media_anim_start(media_anim_t *anim, int32_t from, int32_t to, uint32_t duration_ms);
/** Returns the current value; clears `active` once the animation has completed. */
int32_t media_anim_value(media_anim_t *anim, int64_t now_us);
bool media_anim_running(const media_anim_t *anim);

/** t in 0..65536 -> eased 0..65536. */
int32_t media_ease_out(int32_t t);
int32_t media_ease_smoothstep(int32_t t);

/* -------------------------------------------------------------------------------------- */
/* Misc                                                                                     */
/* -------------------------------------------------------------------------------------- */

/** xorshift32 PRNG - deterministic and allocation free, used by the shuffle bag. */
uint32_t media_rand(uint32_t *state);

static inline int media_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float media_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
