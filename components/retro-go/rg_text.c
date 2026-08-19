/**
 * Arabic/Persian text shaping and a small bidirectional reordering pass.
 *
 * This is deliberately not a full Unicode bidi implementation (UAX #9 is a few thousand lines and
 * needs tables we have no room for). It implements the part that matters for a menu: join Arabic
 * letters into their contextual forms, build the mandatory lam-alef ligatures, and lay a line out
 * right to left while keeping numbers and Latin words inside it running left to right.
 */

#include "rg_system.h"
#include "rg_text.h"

#include <string.h>

#define MAX_CHARS 256 // Longer strings are drawn unshaped rather than truncated
#define MAX_BYTES (MAX_CHARS * 4 + 1)

/* Contextual forms of one letter. A zero means the letter has no such form, which is also how the
 * joining class is derived: a letter with an initial form joins on both sides, one with only a
 * final form joins to the letter before it, and one with neither does not join at all. */
typedef struct
{
    uint16_t base;
    uint16_t isolated, final, initial, medial;
} arabic_forms_t;

static const arabic_forms_t arabic_forms[] = {
    // Arabic, mapped into Arabic Presentation Forms-B
    {0x0621, 0xFE80, 0x0000, 0x0000, 0x0000}, // HAMZA
    {0x0622, 0xFE81, 0xFE82, 0x0000, 0x0000}, // ALEF WITH MADDA
    {0x0623, 0xFE83, 0xFE84, 0x0000, 0x0000}, // ALEF WITH HAMZA ABOVE
    {0x0624, 0xFE85, 0xFE86, 0x0000, 0x0000}, // WAW WITH HAMZA
    {0x0625, 0xFE87, 0xFE88, 0x0000, 0x0000}, // ALEF WITH HAMZA BELOW
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, // YEH WITH HAMZA
    {0x0627, 0xFE8D, 0xFE8E, 0x0000, 0x0000}, // ALEF
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, // BEH
    {0x0629, 0xFE93, 0xFE94, 0x0000, 0x0000}, // TEH MARBUTA
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98}, // TEH
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, // THEH
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, // JEEM
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, // HAH
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, // KHAH
    {0x062F, 0xFEA9, 0xFEAA, 0x0000, 0x0000}, // DAL
    {0x0630, 0xFEAB, 0xFEAC, 0x0000, 0x0000}, // THAL
    {0x0631, 0xFEAD, 0xFEAE, 0x0000, 0x0000}, // REH
    {0x0632, 0xFEAF, 0xFEB0, 0x0000, 0x0000}, // ZAIN
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, // SEEN
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, // SHEEN
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, // SAD
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, // DAD
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, // TAH
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, // ZAH
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC}, // AIN
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, // GHAIN
    {0x0640, 0x0640, 0x0640, 0x0640, 0x0640}, // TATWEEL (joins on both sides, one shape)
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4}, // FEH
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, // QAF
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, // KAF
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, // LAM
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, // MEEM
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, // NOON
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, // HEH
    {0x0648, 0xFEED, 0xFEEE, 0x0000, 0x0000}, // WAW
    {0x0649, 0xFEEF, 0xFEF0, 0x0000, 0x0000}, // ALEF MAKSURA
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}, // YEH

    // Persian and Urdu letters, mapped into Arabic Presentation Forms-A
    {0x0679, 0xFB66, 0xFB67, 0xFB68, 0xFB69}, // TTEH
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59}, // PEH
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D}, // TCHEH
    {0x0688, 0xFB88, 0xFB89, 0x0000, 0x0000}, // DDAL
    {0x0691, 0xFB8C, 0xFB8D, 0x0000, 0x0000}, // RREH
    {0x0698, 0xFB8A, 0xFB8B, 0x0000, 0x0000}, // JEH
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91}, // KEHEH (Persian kaf)
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95}, // GAF
    {0x06BA, 0xFB9E, 0xFB9F, 0x0000, 0x0000}, // NOON GHUNNA
    {0x06BE, 0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD}, // HEH DOACHASHMEE
    {0x06C0, 0xFBA4, 0xFBA5, 0x0000, 0x0000}, // HEH WITH YEH ABOVE
    {0x06C1, 0xFBA6, 0xFBA7, 0xFBA8, 0xFBA9}, // HEH GOAL
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF}, // FARSI YEH
    {0x06D2, 0xFBAE, 0xFBAF, 0x0000, 0x0000}, // YEH BARREE
    {0x06D3, 0xFBB0, 0xFBB1, 0x0000, 0x0000}, // YEH BARREE WITH HAMZA
};

/* Lam followed by one of these alefs is a required ligature, not two glyphs side by side. */
static const struct
{
    uint16_t alef;
    uint16_t isolated, final;
} lam_alef[] = {
    {0x0622, 0xFEF5, 0xFEF6},
    {0x0623, 0xFEF7, 0xFEF8},
    {0x0625, 0xFEF9, 0xFEFA},
    {0x0627, 0xFEFB, 0xFEFC},
};

/* Character classes, only as many as the reordering below actually distinguishes. */
enum
{
    CLASS_NEUTRAL = 0,
    CLASS_LTR,
    CLASS_RTL,
    CLASS_DIGIT, // Reads left to right even inside right-to-left text
};

static const arabic_forms_t *find_forms(uint32_t codepoint)
{
    for (size_t i = 0; i < RG_COUNT(arabic_forms); ++i)
        if (arabic_forms[i].base == codepoint)
            return &arabic_forms[i];
    return NULL;
}

/* Marks and vowel signs: they sit on top of a letter and must not break a join. */
static bool is_transparent(uint32_t c)
{
    return (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||
           (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x06DF && c <= 0x06E4) || c == 0x06E7 || c == 0x06E8 ||
           (c >= 0x06EA && c <= 0x06ED) || c == 0x200B;
}

static bool is_rtl_char(uint32_t c)
{
    return (c >= 0x0590 && c <= 0x05FF) ||   // Hebrew
           (c >= 0x0600 && c <= 0x06FF) ||   // Arabic
           (c >= 0x0750 && c <= 0x077F) ||   // Arabic Supplement
           (c >= 0x08A0 && c <= 0x08FF) ||   // Arabic Extended-A
           (c >= 0xFB1D && c <= 0xFDFF) ||   // Hebrew and Arabic presentation forms
           (c >= 0xFE70 && c <= 0xFEFF) ||   // Arabic presentation forms-B
           c == 0x200F;                      // RLM
}

static int classify(uint32_t c)
{
    if (is_rtl_char(c))
    {
        // Arabic-Indic and Persian digits are written left to right like any other number
        if ((c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9))
            return CLASS_DIGIT;
        return CLASS_RTL;
    }
    if (c >= '0' && c <= '9')
        return CLASS_DIGIT;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0x00C0 && c <= 0x024F) ||
        (c >= 0x0370 && c <= 0x052F) || c >= 0x1E00)
        return CLASS_LTR;
    return CLASS_NEUTRAL;
}

static void reverse_range(uint32_t *chars, int from, int to)
{
    while (from < to)
    {
        uint32_t tmp = chars[from];
        chars[from++] = chars[to];
        chars[to--] = tmp;
    }
}

/**
 * Replace every Arabic letter with the shape its neighbours call for.
 *
 * Joining is decided on the logical string (before any reordering) and looks through marks in both
 * directions, which is why the two neighbour searches skip transparent characters.
 */
static int shape_arabic(uint32_t *chars, int count)
{
    uint32_t out[MAX_CHARS];
    int out_count = 0;

    for (int i = 0; i < count; ++i)
    {
        const arabic_forms_t *forms = find_forms(chars[i]);

        if (!forms)
        {
            out[out_count++] = chars[i];
            continue;
        }

        // Previous letter: does it join forwards? (it must have an initial or medial form)
        bool join_before = false;
        for (int j = i - 1; j >= 0; --j)
        {
            if (is_transparent(chars[j]))
                continue;
            if (chars[j] == 0x200C) // ZWNJ explicitly forbids the join
                break;
            const arabic_forms_t *prev = find_forms(chars[j]);
            join_before = prev && prev->initial != 0;
            break;
        }

        // Next letter: can it be joined to from the right? (it must have a final form)
        bool join_after = false;
        uint32_t next_char = 0;
        for (int j = i + 1; j < count; ++j)
        {
            if (is_transparent(chars[j]))
                continue;
            if (chars[j] == 0x200C)
                break;
            const arabic_forms_t *next = find_forms(chars[j]);
            join_after = next && next->final != 0;
            next_char = chars[j];
            break;
        }

        // Lam + alef is one glyph. Its shape depends on what precedes the lam, and the alef is
        // consumed, so the loop skips it.
        if (chars[i] == 0x0644 && next_char)
        {
            for (size_t k = 0; k < RG_COUNT(lam_alef); ++k)
            {
                if (lam_alef[k].alef != next_char)
                    continue;
                out[out_count++] = join_before ? lam_alef[k].final : lam_alef[k].isolated;
                // Skip the alef we just absorbed, and any marks between it and the lam
                for (int j = i + 1; j < count; ++j)
                {
                    if (chars[j] == next_char)
                    {
                        i = j;
                        break;
                    }
                }
                forms = NULL;
                break;
            }
            if (!forms)
                continue;
        }

        uint32_t shaped;
        if (join_before && join_after && forms->medial)
            shaped = forms->medial;
        else if (join_before && forms->final)
            shaped = forms->final;
        else if (join_after && forms->initial)
            shaped = forms->initial;
        else
            shaped = forms->isolated;

        out[out_count++] = shaped ? shaped : chars[i];
    }

    memcpy(chars, out, out_count * sizeof(uint32_t));
    return out_count;
}

/**
 * Put the line in display order.
 *
 * A line whose first strong character is right-to-left is laid out right to left: reversing the
 * whole line puts it in visual order, and any run of Latin or digits inside it is reversed back so
 * it reads normally again. A line that is mostly left-to-right only needs its right-to-left runs
 * flipped. Neutral characters (spaces, punctuation) follow whatever is on both sides of them, and
 * the paragraph direction when the two disagree.
 */
static void reorder_bidi(uint32_t *chars, int count)
{
    uint8_t classes[MAX_CHARS];
    bool paragraph_rtl = false;
    bool have_strong = false;

    for (int i = 0; i < count; ++i)
    {
        classes[i] = classify(chars[i]);
        if (!have_strong && (classes[i] == CLASS_RTL || classes[i] == CLASS_LTR))
        {
            paragraph_rtl = classes[i] == CLASS_RTL;
            have_strong = true;
        }
    }

    if (!have_strong)
        return;

    // Resolve neutrals from their surroundings
    for (int i = 0; i < count; ++i)
    {
        if (classes[i] != CLASS_NEUTRAL)
            continue;

        int before = CLASS_NEUTRAL, after = CLASS_NEUTRAL;
        for (int j = i - 1; j >= 0; --j)
            if (classes[j] != CLASS_NEUTRAL)
            {
                before = classes[j] == CLASS_DIGIT ? (paragraph_rtl ? CLASS_RTL : CLASS_LTR) : classes[j];
                break;
            }
        for (int j = i + 1; j < count; ++j)
            if (classes[j] != CLASS_NEUTRAL)
            {
                after = classes[j] == CLASS_DIGIT ? (paragraph_rtl ? CLASS_RTL : CLASS_LTR) : classes[j];
                break;
            }

        classes[i] = (before == after && before != CLASS_NEUTRAL) ? before
                                                                  : (paragraph_rtl ? CLASS_RTL : CLASS_LTR);
    }

    if (paragraph_rtl)
    {
        reverse_range(chars, 0, count - 1);

        // The classes were computed in logical order, so mirror them too before walking the runs
        for (int i = 0, j = count - 1; i < j; ++i, --j)
        {
            uint8_t tmp = classes[i];
            classes[i] = classes[j];
            classes[j] = tmp;
        }

        for (int i = 0; i < count;)
        {
            if (classes[i] == CLASS_RTL)
            {
                i++;
                continue;
            }
            int start = i;
            while (i < count && classes[i] != CLASS_RTL)
                i++;
            reverse_range(chars, start, i - 1);
        }
    }
    else
    {
        for (int i = 0; i < count;)
        {
            if (classes[i] != CLASS_RTL)
            {
                i++;
                continue;
            }
            int start = i;
            while (i < count && (classes[i] == CLASS_RTL || classes[i] == CLASS_DIGIT))
            {
                // A number at the end of a right-to-left run belongs to what follows it
                if (classes[i] == CLASS_DIGIT)
                {
                    int j = i;
                    while (j < count && classes[j] == CLASS_DIGIT)
                        j++;
                    if (j >= count || classes[j] != CLASS_RTL)
                        break;
                }
                i++;
            }
            reverse_range(chars, start, i - 1);

            // Digits inside the run keep their own order
            for (int j = start; j < i;)
            {
                if (classes[count - 1 - j] == CLASS_DIGIT) // classes are still in logical order here
                {
                    int k = j;
                    while (k < i && chars[k] >= '0' && chars[k] <= '9')
                        k++;
                    reverse_range(chars, j, k - 1);
                    j = k;
                }
                else
                {
                    j++;
                }
            }
        }
    }
}

bool rg_text_is_rtl(const char *text)
{
    if (!text)
        return false;

    for (const char *ptr = text; *ptr;)
    {
        int c = rg_utf8_decode(&ptr);
        int class = classify(c);
        if (class == CLASS_RTL)
            return true;
        if (class == CLASS_LTR)
            return false;
    }

    return false;
}

const char *rg_text_shape(const char *text)
{
    static char output[MAX_BYTES];

    if (!text || !*text)
        return text;

    // Fast path: no Arabic or Hebrew lead byte means there is nothing to do, and this runs on every
    // string the GUI draws.
    bool candidate = false;
    size_t length = 0;
    for (const unsigned char *ptr = (const unsigned char *)text; *ptr; ++ptr, ++length)
    {
        if (*ptr >= 0xD6 && *ptr <= 0xDB) // U+0590..U+06FF, U+0750..
            candidate = true;
        else if (*ptr == 0xE0 || *ptr == 0xEF) // U+0800.., U+F000..
            candidate = true;
    }

    if (!candidate || length >= MAX_BYTES)
        return text;

    uint32_t chars[MAX_CHARS];
    int count = 0;
    const char *lines[MAX_CHARS];

    (void)lines;

    // Shape and reorder one line at a time: a line break resets the direction, and reversing across
    // one would move text onto the wrong line.
    char *out = output;
    const char *ptr = text;

    while (*ptr && (out - output) < MAX_BYTES - 8)
    {
        count = 0;
        while (*ptr && *ptr != '\n' && count < MAX_CHARS)
            chars[count++] = rg_utf8_decode(&ptr);

        count = shape_arabic(chars, count);
        reorder_bidi(chars, count);

        for (int i = 0; i < count && (out - output) < MAX_BYTES - 8; ++i)
            out += rg_utf8_encode(out, chars[i]);

        if (*ptr == '\n')
        {
            *out++ = *ptr++;
            // Consume any remaining characters of an over-long line so the newline stays in sync
        }
    }

    *out = 0;
    return output;
}
