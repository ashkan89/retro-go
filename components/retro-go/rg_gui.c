#include "rg_system.h"
#include "rg_gui.h"
#include "rg_usb_hid.h"
#include "rg_usb_xinput.h"
#include "rg_usb_msc.h"

#include <cJSON.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "rg_text.h"

#include "bitmaps/image_hourglass.h"
#include "fonts/fonts.h"

/**
 * Where drawing goes.
 *
 * `buffer` NULL means "straight to the panel" (an emulator's menu overlay). Otherwise every draw
 * lands in that buffer, which may be the whole screen (the launcher and media player each own a
 * surface) or a small window of it (a dialog composited offscreen, see begin_offscreen). Keeping
 * the origin and stride here is what lets the same drawing code do both without knowing which.
 */
typedef struct
{
    uint16_t *buffer;
    int left, top, width, height; // Screen coordinates covered by the buffer
    int stride;                   // In pixels
} rg_gui_target_t;

static struct
{
    uint16_t *screen_buffer, *draw_buffer;
    rg_gui_target_t target;
    uint16_t *composite_buffer;
    size_t composite_buffer_size;
    const rg_surface_t *backdrop;
    rg_rect_t last_overlay;
    rg_gui_target_t overlay_saved; // Target to restore when the public overlay ends
    bool overlay_active;
    int dialog_top; // First visible row of the dialog currently on screen
    size_t draw_buffer_size;
    int screen_width, screen_height;
    rg_margins_t margins;
    struct
    {
        rg_color_t box_background;
        rg_color_t box_header;
        rg_color_t box_border;
        rg_color_t item_standard;
        rg_color_t item_disabled;
        rg_color_t item_message;
        rg_color_t scrollbar;
        rg_color_t shadow;
        rg_color_t item_value;
    } style;
    rg_gui_palette_t palette;
    char theme_name[32];
    cJSON *theme_obj;
    const rg_font_t *font;
    int font_index;
    int font_height;
    bool show_clock;
    bool initialized;
} gui;

#define SETTING_FONTTYPE    "FontType"
#define SETTING_CLOCK       "Clock"
#define SETTING_THEME       "Theme"
#define SETTING_WIFI_ENABLE "Enable"
#define SETTING_WIFI_SLOT   "Slot"
#define SETTING_LANGUAGE    "Language"

static uint16_t *get_draw_buffer(int width, int height, rg_color_t fill_color)
{
    size_t pixels = width * height;
    if (pixels > gui.draw_buffer_size)
    {
        if (gui.draw_buffer != NULL)
        {
            RG_LOGW("Growing drawing buffer to %dx%d...", width, height);
            free(gui.draw_buffer);
        }
        gui.draw_buffer = rg_alloc(pixels * 2, MEM_SLOW);
        gui.draw_buffer_size = pixels;
    }

    if (!gui.draw_buffer)
        RG_PANIC("Failed to allocate draw buffer!");

    if (fill_color != C_NONE)
    {
        for (size_t i = 0; i < pixels; ++i)
            gui.draw_buffer[i] = fill_color;
    }

    return gui.draw_buffer;
}

static int get_horizontal_position(int x_pos, int width)
{
    int type = (x_pos & 0xFF0000) | 0x8000;
    int offset = (x_pos & 0xFFFF) - 0x8000;
    if (type == RG_GUI_CENTER)
        return ((gui.screen_width - width) / 2) + offset;
    else if (type == RG_GUI_LEFT)
        return 0 + offset;
    else if (type == RG_GUI_RIGHT)
        return (gui.screen_width - width) - offset;
    else if (x_pos < 0)
        return x_pos + gui.screen_width;
    return x_pos;
}

static int get_vertical_position(int y_pos, int height)
{
    int type = (y_pos & 0xFF0000) | 0x8000;
    int offset = (y_pos & 0xFFFF) - 0x8000;
    if (type == RG_GUI_CENTER)
        return ((gui.screen_height - height) / 2) + offset;
    else if (type == RG_GUI_TOP)
        return 0 + offset;
    else if (type == RG_GUI_BOTTOM)
        return (gui.screen_height - height) - offset;
    else if (y_pos < 0)
        return y_pos + gui.screen_height;
    return y_pos;
}

static void gui_update_geometry(void)
{
    gui.screen_width = rg_display_get_width();
    gui.screen_height = rg_display_get_height();
    // FIXME: RG_SCREEN_SAFE_AREA being added on top of RG_SCREEN_VISIBLE_AREA might not be super intuitive
    //        because of how this is defined in config.h. It should be documented somewhere...
    gui.margins = (rg_margins_t)RG_SCREEN_SAFE_AREA;
    gui.draw_buffer = get_draw_buffer(gui.screen_width, 18, C_BLACK);
}

void rg_gui_init(void)
{
    gui_update_geometry();
    gui.show_clock = rg_settings_get_boolean(NS_GLOBAL, SETTING_CLOCK, false);
    if (!rg_gui_set_language_id(rg_settings_get_number(NS_GLOBAL, SETTING_LANGUAGE, RG_LANG_DEFAULT)))
        rg_gui_set_language_id(0);
    if (!rg_gui_set_font(rg_settings_get_number(NS_GLOBAL, SETTING_FONTTYPE, RG_FONT_DEFAULT)))
        rg_gui_set_font(0);
    rg_gui_set_theme(rg_settings_get_string(NS_GLOBAL, SETTING_THEME, NULL));
    gui.initialized = true;
}

void rg_gui_update_geometry(void)
{
    if (gui.initialized)
        gui_update_geometry();
}

bool rg_gui_set_language_id(int index)
{
    if (rg_localization_set_language_id(index))
    {
        rg_settings_set_number(NS_GLOBAL, SETTING_LANGUAGE, index);
        RG_LOGI("Language set to: %s (%d)", rg_localization_get_language_name(index), index);
        return true;
    }
    RG_LOGE("Invalid language id %d!", index);
    return false;
}

bool rg_gui_set_theme(const char *theme_name)
{
    char pathbuf[RG_PATH_MAX];
    cJSON *new_theme = NULL;

    // Cleanup the current theme
    cJSON_Delete(gui.theme_obj);
    gui.theme_obj = NULL;

    if (theme_name && theme_name[0])
    {
        snprintf(pathbuf, RG_PATH_MAX, "%s/%s/theme.json", RG_BASE_PATH_THEMES, theme_name);
        char *data;
        size_t data_len;
        if (rg_storage_read_file(pathbuf, (void **)&data, &data_len, 0))
        {
            new_theme = cJSON_Parse(data);
            if (!new_theme) // Parse failure, clean the markup and try again
                new_theme = cJSON_Parse(rg_json_fixup(data));
            free(data);
        }
        if (!new_theme)
            RG_LOGE("Failed to load theme JSON from '%s'!\n", pathbuf);
    }

    if (new_theme)
    {
        rg_settings_set_string(NS_GLOBAL, SETTING_THEME, theme_name);
        strcpy(gui.theme_name, theme_name);
        // FIXME: Keeping the theme around uses quite a lot of internal memory (about 3KB)...
        //        We should probably convert it to a regular array or hashmap.
        gui.theme_obj = new_theme;
        RG_LOGI("Theme set to '%s'!\n", theme_name);
    }
    else
    {
        rg_settings_set_string(NS_GLOBAL, SETTING_THEME, NULL);
        strcpy(gui.theme_name, "");
        gui.theme_obj = NULL;
        RG_LOGI("Using built-in theme!\n");
    }

    // The classic keys keep their names and meaning so every existing theme still applies
    gui.style.box_background = rg_gui_get_theme_color("dialog", "background", C_RGB(17, 18, 24));
    gui.style.box_header = rg_gui_get_theme_color("dialog", "header", C_RGB(240, 243, 250));
    gui.style.box_border = rg_gui_get_theme_color("dialog", "border", C_RGB(62, 66, 82));
    gui.style.item_standard = rg_gui_get_theme_color("dialog", "item_standard", C_RGB(232, 236, 245));
    gui.style.item_disabled = rg_gui_get_theme_color("dialog", "item_disabled", C_RGB(120, 126, 140));
    gui.style.item_message = rg_gui_get_theme_color("dialog", "item_message", C_RGB(176, 182, 198));
    gui.style.scrollbar = rg_gui_get_theme_color("dialog", "scrollbar", C_RGB(90, 170, 255));

    // Everything below is derived from the keys above unless the theme says otherwise, so a
    // theme written for an older version gets a coherent palette (its own background tinted for
    // the surfaces, its own text dimmed for secondary text) instead of colors that clash with it.
    rg_gui_palette_t *palette = &gui.palette;

    palette->background = gui.style.box_background;
    palette->text = gui.style.item_standard;
    palette->border = gui.style.box_border;
    palette->accent = rg_gui_get_theme_color("dialog", "accent", C_RGB(90, 170, 255));
    palette->accent_dim =
        rg_gui_get_theme_color("dialog", "accent_dim", rg_gui_blend_color(palette->background, palette->accent, 96));
    palette->highlight =
        rg_gui_get_theme_color("dialog", "highlight", rg_gui_blend_color(palette->text, C_WHITE, 140));
    palette->surface =
        rg_gui_get_theme_color("dialog", "surface", rg_gui_blend_color(palette->background, C_WHITE, 26));
    palette->surface_alt =
        rg_gui_get_theme_color("dialog", "surface_alt", rg_gui_blend_color(palette->background, palette->accent, 34));
    palette->divider =
        rg_gui_get_theme_color("dialog", "divider", rg_gui_blend_color(palette->background, C_WHITE, 62));
    palette->text_dim = rg_gui_get_theme_color("dialog", "text_dim", gui.style.item_disabled);
    // A shadow needs to be darker than the background to read as one, so the default is derived
    // from it rather than being a fixed color. A theme can still opt out with "shadow": "none".
    gui.style.shadow = rg_gui_get_theme_color("dialog", "shadow", rg_gui_scale_color(palette->background, 40));
    palette->shadow = gui.style.shadow;

    // Values sit next to their label and should read as secondary information, not as a second
    // label competing with it.
    gui.style.item_value = rg_gui_get_theme_color("dialog", "item_value",
                                                  rg_gui_blend_color(palette->text, palette->background, 90));

    return true;
}

int rg_gui_get_font_height(void)
{
    return gui.font_height;
}

rg_color_t rg_gui_get_theme_color(const char *section, const char *key, rg_color_t default_value)
{
    cJSON *root = section ? cJSON_GetObjectItem(gui.theme_obj, section) : gui.theme_obj;
    cJSON *obj = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(obj))
        return obj->valueint;
    char *strval = cJSON_GetStringValue(obj);
    if (!strval || strlen(strval) < 4)
        return default_value;
    if (strcmp(strval, "transparent") == 0)
        return C_TRANSPARENT;
    if (strcmp(strval, "none") == 0)
        return C_NONE;
    int intval = (int)strtol(strval, NULL, 0);
    // It is better to specify colors as RGB565 to avoid data loss, but we also accept RGB888 for convenience
    if (strlen(strval) == 8 && strval[0] == '0' && strval[1] == 'x')
        return (((intval >> 19) & 0x1F) << 11) | (((intval >> 10) & 0x3F) << 5) | (((intval >> 3) & 0x1F));
    return intval;
}

rg_image_t *rg_gui_get_theme_image(const char *name)
{
    char pathbuf[RG_PATH_MAX];
    if (!name || !gui.theme_name[0])
        return NULL;
    snprintf(pathbuf, RG_PATH_MAX, "%s/%s/%s", RG_BASE_PATH_THEMES, gui.theme_name, name);
    return rg_surface_load_image_file(pathbuf, 0);
}

const char *rg_gui_get_theme_name(void)
{
    return gui.theme_name[0] ? gui.theme_name : NULL;
}

bool rg_gui_set_font(int index)
{
    if (index < 0 || index > RG_FONT_MAX - 1)
        return false;

    gui.font = fonts[index];
    gui.font_index = index;
    gui.font_height = (index < 3) ? (8 + index * 4) : gui.font->height;

    rg_settings_set_number(NS_GLOBAL, SETTING_FONTTYPE, index);

    RG_LOGI("Font set to: %s (height=%d, scaling=%.2f)\n", gui.font->name, gui.font_height,
            (float)gui.font_height / gui.font->height);

    return true;
}

void rg_gui_set_surface(rg_surface_t *surface)
{
    // Whoever last drew a full frame into a surface is, by definition, what the panel is showing,
    // so that surface also becomes the backdrop our overlays composite over. Setting the surface
    // back to NULL (which every app does at the end of its redraw) deliberately does not clear it.
    // A surface that is about to be freed must be dropped with rg_gui_set_backdrop(NULL) first.
    if (surface)
        rg_gui_set_backdrop(surface);

    gui.screen_buffer = surface ? surface->data : NULL;
    gui.target = (rg_gui_target_t){
        .buffer = gui.screen_buffer,
        .left = 0,
        .top = 0,
        .width = gui.screen_width,
        .height = gui.screen_height,
        .stride = gui.screen_width,
    };
}

/**
 * Tell the GUI what is behind an overlay, so dialogs can be composited over it instead of being
 * painted onto the panel piece by piece (which is what produces the flicker).
 *
 * The launcher passes the surface it renders into: its content is by definition what the panel is
 * currently showing. An emulator has nothing to offer here - its frame lives in the display task's
 * scaling pipeline - so it passes nothing and dialogs fall back to compositing over a flat plate.
 */
void rg_gui_set_backdrop(const rg_surface_t *surface)
{
    gui.backdrop = (surface && surface->width == gui.screen_width && surface->height == gui.screen_height &&
                    (surface->format & RG_PIXEL_FORMAT) == RG_PIXEL_565_LE)
                       ? surface
                       : NULL;
}

/**
 * Redirect drawing into a scratch buffer covering `rect`, pre-filled with what is behind it.
 *
 * `seed` is the color to fill it with; C_NONE means "use the registered backdrop, or a dark plate
 * if there is none". A screen that paints its own background (the on-screen keyboard) has to pass
 * that background color instead, because the backdrop describes the screen underneath it, not the
 * one being drawn.
 */
static bool begin_offscreen(rg_rect_t rect, rg_color_t seed, rg_gui_target_t *saved)
{
    if (gui.target.buffer) // Already drawing into a buffer, nothing to composite
        return false;

    rect.left = RG_MAX(rect.left, 0);
    rect.top = RG_MAX(rect.top, 0);
    rect.width = RG_MIN(rect.width, gui.screen_width - rect.left);
    rect.height = RG_MIN(rect.height, gui.screen_height - rect.top);

    if (rect.width < 1 || rect.height < 1)
        return false;

    size_t pixels = (size_t)rect.width * rect.height;

    if (pixels > gui.composite_buffer_size)
    {
        // Grow once and keep it: a dialog is redrawn on every keypress
        uint16_t *buffer = rg_alloc(pixels * 2, MEM_SLOW);
        if (!buffer)
        {
            // Not enough memory: fall back to drawing straight to the panel. It flickers, but a
            // menu that works beats a menu that does not appear.
            RG_LOGW("Failed to allocate %dx%d composite buffer", rect.width, rect.height);
            return false;
        }
        free(gui.composite_buffer);
        gui.composite_buffer = buffer;
        gui.composite_buffer_size = pixels;
    }

    *saved = gui.target;
    gui.target = (rg_gui_target_t){
        .buffer = gui.composite_buffer,
        .left = rect.left,
        .top = rect.top,
        .width = rect.width,
        .height = rect.height,
        .stride = rect.width,
    };

    if (seed != C_NONE)
    {
        for (size_t i = 0; i < pixels; ++i)
            gui.composite_buffer[i] = seed;
    }
    else if (gui.backdrop)
    {
        const uint16_t *src = gui.backdrop->data;
        int src_stride = gui.backdrop->stride / 2;
        for (int y = 0; y < rect.height; ++y)
            memcpy(gui.composite_buffer + y * rect.width, src + (rect.top + y) * src_stride + rect.left,
                   rect.width * 2);
    }
    else
    {
        // No backdrop to composite over, so we provide one: a dark plate the card and its
        // translucent edges can sit on. It is what makes the overlay look deliberate over a game
        // frame rather than like a rectangle of garbage.
        rg_color_t plate = rg_gui_blend_color(gui.style.box_background, C_BLACK, 110);
        for (size_t i = 0; i < pixels; ++i)
            gui.composite_buffer[i] = plate;
    }

    return true;
}

/* Send the composited window to the panel in a single transfer and restore the previous target. */
static void end_offscreen(const rg_gui_target_t *saved)
{
    rg_gui_target_t done = gui.target;
    gui.target = *saved;
    rg_display_write_rect(done.left, done.top, done.width, done.height, done.stride * 2, done.buffer, 0);
}

/**
 * Composite anything, not just a dialog: everything drawn until rg_gui_end_overlay() lands in a
 * scratch buffer covering `rect` and reaches the panel as one transfer.
 *
 * Returns false when it could not be set up (not enough memory, or drawing already goes to a
 * buffer), in which case the caller should just draw normally - the result is the same, only less
 * smooth. Overlays do not nest.
 */
bool rg_gui_begin_overlay(int x_pos, int y_pos, int width, int height, rg_color_t seed)
{
    if (gui.overlay_active)
        return false;

    rg_rect_t rect = {get_horizontal_position(x_pos, width), get_vertical_position(y_pos, height), width, height};

    gui.overlay_active = begin_offscreen(rect, seed, &gui.overlay_saved);
    return gui.overlay_active;
}

void rg_gui_end_overlay(void)
{
    if (!gui.overlay_active)
        return;

    gui.overlay_active = false;
    end_offscreen(&gui.overlay_saved);
}

rg_margins_t rg_gui_get_safe_area(void)
{
    return gui.margins;
}

void rg_gui_copy_buffer(int left, int top, int width, int height, int stride, const uint16_t *buffer, bool transparency)
{
    left = get_horizontal_position(left, width);
    top = get_vertical_position(top, height);
    width = RG_MIN(width, gui.screen_width - left);
    height = RG_MIN(height, gui.screen_height - top);

    if (width <= 0 || height <= 0)
        return;

    if (left >= gui.screen_width || top >= gui.screen_height)
    {
        RG_LOGD("Buffer (x: %d, y:%d) is entirely outside the screen!", left, top);
        return;
    }

    if (gui.target.buffer)
    {
        if (stride < width)
            stride = width * 2;

        // Clip to the target window. A dialog composited offscreen only owns its own rectangle, and
        // text or an image that reaches past it must be cut, not wrapped onto the next row.
        int skip_x = RG_MAX(gui.target.left - left, 0);
        int skip_y = RG_MAX(gui.target.top - top, 0);
        int max_width = (gui.target.left + gui.target.width) - (left + skip_x);
        int max_height = (gui.target.top + gui.target.height) - (top + skip_y);
        int copy_width = RG_MIN(width - skip_x, max_width);
        int copy_height = RG_MIN(height - skip_y, max_height);

        // Entirely outside the target window. This has to be checked before the copy below, because
        // a negative width reaching memcpy() would be read as an enormous unsigned size.
        if (copy_width <= 0 || copy_height <= 0)
            return;

        for (int y = 0; y < copy_height; ++y)
        {
            uint16_t *dst = gui.target.buffer + (top + skip_y + y - gui.target.top) * gui.target.stride +
                            (left + skip_x - gui.target.left);
            const uint16_t *src = (void *)buffer + (y + skip_y) * stride + skip_x * 2;
            if (transparency)
            {
                for (int x = 0; x < copy_width; ++x)
                    if (src[x] != C_TRANSPARENT)
                        dst[x] = src[x];
            }
            else
            {
                memcpy(dst, src, copy_width * 2);
            }
        }
    }
    else
    {
        rg_display_write_rect(left, top, width, height, stride, buffer, 0);
    }
}

static size_t get_glyph(uint32_t *output, const rg_font_t *font, int points, int c)
{
    // Some glyphs are always zero width
    if (!font || c == '\r' || c == '\n' || c == 0) // || c < 8 || c > 0xFFFF)
        return 0;

    if (points <= 0)
        points = font->height;

    const uint8_t *ptr = font->data;
    const rg_font_glyph_t *glyph = (rg_font_glyph_t *)ptr;
    // for (size_t i = 0; i < font->chars && glyph->code && glyph->code != c; ++i)
    while (glyph->code && glyph->code != c)
    {
        if (glyph->width != 0)
            ptr += (((glyph->width * glyph->height) - 1) / 8) + 1;
        ptr += sizeof(rg_font_glyph_t);
        glyph = (rg_font_glyph_t *)ptr;
    }

    if (glyph && glyph->code == c) // Glyph found
    {
        // Based on code by Boris Lovosevic (https://github.com/loboris)
        int yOffset = glyph->yOffset;
        int width = glyph->width;
        int height = glyph->height;
        int xOffset = glyph->xOffset < 0x80 ? glyph->xOffset : -(0xFF - glyph->xOffset);
        int xDelta = glyph->xDelta;
        const uint8_t *data = glyph->data;
        if (output)
        {
            memset(output, 0, points * 4);
            int ch = 0, mask = 0x80;
            for (int y = 0; y < height; y++)
            {
                uint32_t row = 0;
                for (int x = 0; x < width; x++)
                {
                    if (((x + (y * width)) % 8) == 0)
                    {
                        mask = 0x80;
                        ch = *data++;
                    }
                    if ((ch & mask) != 0)
                        row |= (1 << (xOffset + x));
                    mask >>= 1;
                }
                output[yOffset + y] = row;
            }
            // Vertical stretching
            if (points != font->height)
            {
                float scale = (float)points / font->height;
                for (int y = points - 1; y >= 0; y--)
                    output[y] = output[(int)(y / scale)];
            }
        }
        return RG_MAX(width, xDelta);
    }
    // else if (font != &font_basic8x8) // Glyph not found, try fallback font
    // {
    //     return get_glyph(output, &font_basic8x8, points, c);
    // }
    else // Glyph not found, no fallback
    {
        size_t box_width = font->width ?: 8;
        if (output) // draw missing box
        {
            uint32_t mask = ~((0xFFFFFFFF << (box_width - 1)) | 1);
            for (size_t i = 0; i < points; ++i)
                output[i] = (0xAAAAAAAA << (i & 1)) & mask;
        }
        return box_width;
    }
}

rg_rect_t rg_gui_draw_text(int x_pos, int y_pos, int width, const char *text, // const rg_font_t *font,
                           rg_color_t color_fg, rg_color_t color_bg, uint32_t flags)
{
    const rg_font_t *font = gui.font;
    int padding = (flags & RG_TEXT_NO_PADDING) ? 0 : 1;
    int font_height = (flags & RG_TEXT_BIGGER) ? gui.font_height * 2 : gui.font_height;
    int monospace = ((flags & RG_TEXT_MONOSPACE) || font->type == 0) ? font->width : 0;
    int line_height = font_height + padding * 2;
    int line_count = 0;
    bool transparency = color_fg == C_TRANSPARENT || color_bg == C_TRANSPARENT;
    // int16_t line_breaks[64], line_width_cache[64];

    if (!text || *text == 0)
        text = " ";

    // Arabic and Persian need their letters joined and the line laid out right to left before any of
    // the measuring below makes sense. Text with no right-to-left characters comes back unchanged,
    // so this costs one scan of the string.
    text = rg_text_shape(text);

    if (width == 0)
    {
        // Find the longest line to determine our box width
        int line_width = padding * 2;
        for (const char *ptr = text; *ptr;)
        {
            int chr = rg_utf8_decode(&ptr);
            line_width += monospace ?: get_glyph(NULL, font, font_height, chr);

            if (chr == '\n' || *ptr == 0)
            {
                width = RG_MAX(line_width, width);
                line_width = padding * 2;
                line_count++;
            }
        }
    }

    x_pos = get_horizontal_position(x_pos, width);
    y_pos = get_vertical_position(y_pos, line_height);

    if (x_pos >= gui.screen_width || y_pos >= gui.screen_height)
    {
        RG_LOGD("Textbox (x: %d, y:%d) is entirely outside the screen!", x_pos, y_pos);
        return (rg_rect_t){x_pos, y_pos, 0, 0};
    }
    else if (x_pos + width > gui.screen_width || y_pos + line_height > gui.screen_height)
    {
        RG_LOGD("Textbox (pos: %dx%d, size: %dx%d) will be truncated!", x_pos, y_pos, width, line_height);
        // return;
    }

    int draw_width = RG_MIN(width, gui.screen_width - x_pos);
    int y_offset = 0;

    for (const char *ptr = text; *ptr;)
    {
        int x_offset = padding;

        if (flags & (RG_TEXT_ALIGN_RIGHT | RG_TEXT_ALIGN_CENTER))
        {
            // Find the current line's text width
            const char *line = ptr;
            while (x_offset < draw_width && *line && *line != '\n')
            {
                int chr = rg_utf8_decode(&line);
                int width = monospace ?: get_glyph(NULL, font, font_height, chr);
                if (draw_width - x_offset < width) // Do not truncate glyphs
                    break;
                x_offset += width;
            }
            if (flags & RG_TEXT_ALIGN_CENTER)
                x_offset = (draw_width - x_offset) / 2;
            else if (flags & RG_TEXT_ALIGN_RIGHT)
                x_offset = draw_width - x_offset;
        }

        uint16_t *draw_buffer = NULL;

        if (!(flags & RG_TEXT_DUMMY_DRAW))
            draw_buffer = get_draw_buffer(draw_width, line_height, color_bg);

        // The line break is left for the outer loop to consume. Letting this loop eat it as a
        // zero-width glyph meant that after a blank line (two breaks in a row) it carried on and
        // drew the *next* line on the same row, at the blank line's centering offset - which is
        // half the box - and wrapped it early. A message with an empty line in it came out ragged
        // and lost its last row off the bottom of the card.
        while (x_offset < draw_width && *ptr != 0 && *ptr != '\n')
        {
            uint32_t bitmap[font_height];
            const char *prev_ptr = ptr;
            int glyph_width = get_glyph(bitmap, font, font_height, rg_utf8_decode(&ptr));
            int width = monospace ?: glyph_width;

            if (draw_width - x_offset < width) // Do not truncate glyphs
            {
                if (flags & RG_TEXT_MULTILINE)
                    ptr = prev_ptr;
                break;
            }

            if (!(flags & RG_TEXT_DUMMY_DRAW))
            {
                for (int y = 0; y < font_height; y++)
                {
                    uint32_t row = bitmap[y];
                    if (row != 0) // get_draw_buffer fills the bg color, nothing to do if row empty
                    {
                        uint16_t *output = &draw_buffer[(draw_width * (y + padding)) + x_offset];
                        for (int x = 0; x < width; x++)
                            output[x] = ((row >> x) & 1) ? color_fg : color_bg;
                    }
                }
            }

            x_offset += width;
        }

        if (!(flags & RG_TEXT_DUMMY_DRAW))
            rg_gui_copy_buffer(x_pos, y_pos + y_offset, draw_width, line_height, 0, draw_buffer, transparency);

        y_offset += line_height;

        if (!(flags & RG_TEXT_MULTILINE))
            break;

        // Exactly one break per row, so an empty line stays an empty row
        if (*ptr == '\n')
            ptr++;
    }

    return (rg_rect_t){x_pos, y_pos, draw_width, y_offset};
}

void rg_gui_draw_rect(int x_pos, int y_pos, int width, int height, int border_size, rg_color_t border_color,
                      rg_color_t fill_color)
{
    if (width <= 0 || height <= 0)
        return;

    x_pos = get_horizontal_position(x_pos, width);
    y_pos = get_vertical_position(y_pos, height);

    if (border_size > 0)
    {
        uint16_t *draw_buffer = get_draw_buffer(border_size, RG_MAX(width, height), border_color);
        bool transparency = border_color == C_TRANSPARENT;

        rg_gui_copy_buffer(x_pos, y_pos, width, border_size, 0, draw_buffer, transparency); // Top
        rg_gui_copy_buffer(x_pos, y_pos + height - border_size, width, border_size, 0, draw_buffer,
                           transparency);                                                    // Bottom
        rg_gui_copy_buffer(x_pos, y_pos, border_size, height, 0, draw_buffer, transparency); // Left
        rg_gui_copy_buffer(x_pos + width - border_size, y_pos, border_size, height, 0, draw_buffer,
                           transparency); // Right

        x_pos += border_size;
        y_pos += border_size;
        width -= border_size * 2;
        height -= border_size * 2;
    }

    if (width > 0 && height > 0 && fill_color != C_NONE)
    {
        uint16_t *draw_buffer = get_draw_buffer(width, RG_MIN(height, 16), fill_color);
        bool transparency = fill_color == C_TRANSPARENT;
        for (int y = 0; y < height; y += 16)
            rg_gui_copy_buffer(x_pos, y_pos + y, width, RG_MIN(height - y, 16), 0, draw_buffer, transparency);
    }
}

/* -------------------------------------------------------------------------------- */
/* -- Color utilities and blended drawing                                        -- */
/* -------------------------------------------------------------------------------- */

static inline void unpack_565(rg_color_t color, int *r, int *g, int *b)
{
    *r = ((color >> 11) & 0x1F) << 3;
    *g = ((color >> 5) & 0x3F) << 2;
    *b = (color & 0x1F) << 3;
}

static inline rg_color_t pack_565(int r, int g, int b)
{
    r = RG_MIN(RG_MAX(r, 0), 255);
    g = RG_MIN(RG_MAX(g, 0), 255);
    b = RG_MIN(RG_MAX(b, 0), 255);
    return (rg_color_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

rg_color_t rg_gui_blend_color(rg_color_t base, rg_color_t over, int alpha)
{
    if (base == C_NONE || base == C_TRANSPARENT)
        return over;
    if (over == C_NONE || over == C_TRANSPARENT)
        return base;

    int ar, ag, ab, br, bg, bb;
    unpack_565(base, &ar, &ag, &ab);
    unpack_565(over, &br, &bg, &bb);
    alpha = RG_MIN(RG_MAX(alpha, 0), 255);
    return pack_565(ar + ((br - ar) * alpha) / 255, ag + ((bg - ag) * alpha) / 255, ab + ((bb - ab) * alpha) / 255);
}

rg_color_t rg_gui_scale_color(rg_color_t color, int scale)
{
    int r, g, b;
    unpack_565(color, &r, &g, &b);
    return pack_565((r * scale) / 255, (g * scale) / 255, (b * scale) / 255);
}

int rg_gui_color_luma(rg_color_t color)
{
    int r, g, b;
    unpack_565(color, &r, &g, &b);
    return (r * 77 + g * 150 + b * 29) >> 8;
}

bool rg_gui_can_blend(void)
{
    // True whenever drawing lands in a buffer we can read back, which now includes a dialog being
    // composited offscreen, not just an app that owns a surface.
    return gui.target.buffer != NULL;
}

const rg_gui_palette_t *rg_gui_get_palette(void)
{
    return &gui.palette;
}

/**
 * Fill one horizontal run of pixels, blending it into what is already there when possible.
 *
 * Everything in this file that needs translucency or a shape that is not a rectangle goes
 * through here, so there is a single place that knows about the two very different targets we
 * draw to: an in-memory surface (the launcher and the media player) which we can read back and
 * therefore blend against, and the LCD itself (dialogs over a running game) which we cannot
 * read. In the second case a blended color is composited against the theme background instead,
 * which keeps the same design working without ever reading from the panel.
 */
static void fill_span(int x, int y, int width, rg_color_t color, int alpha)
{
    if (width <= 0 || color == C_NONE || alpha <= 0)
        return;

    if (y < 0 || y >= gui.screen_height)
        return;

    if (x < 0)
        width += x, x = 0;
    if (x + width > gui.screen_width)
        width = gui.screen_width - x;
    if (width <= 0)
        return;

    if (gui.target.buffer)
    {
        // Clip to the target window (see rg_gui_target_t)
        if (y < gui.target.top || y >= gui.target.top + gui.target.height)
            return;

        int from = RG_MAX(x, gui.target.left);
        int to = RG_MIN(x + width, gui.target.left + gui.target.width);

        if (to <= from)
            return;

        uint16_t *dst = gui.target.buffer + (y - gui.target.top) * gui.target.stride + (from - gui.target.left);
        width = to - from;

        if (alpha >= 255)
        {
            for (int i = 0; i < width; ++i)
                dst[i] = color;
        }
        else
        {
            for (int i = 0; i < width; ++i)
                dst[i] = rg_gui_blend_color(dst[i], color, alpha);
        }
    }
    else
    {
        rg_color_t solid = (alpha >= 255) ? color : rg_gui_blend_color(gui.style.box_background, color, alpha);
        uint16_t *buffer = get_draw_buffer(width, 1, solid);
        rg_display_write_rect(x, y, width, 1, 0, buffer, 0);
    }
}

/* Same as fill_span but for a block of rows, batched into a single transfer when it can be. */
static void fill_block(int x, int y, int width, int height, rg_color_t color, int alpha)
{
    if (width <= 0 || height <= 0 || color == C_NONE || alpha <= 0)
        return;

    if (!gui.target.buffer)
    {
        // One windowed transfer instead of one per row. A translucent fill can take this path too
        // because without readback it resolves to a single solid color anyway.
        rg_color_t solid = (alpha >= 255) ? color : rg_gui_blend_color(gui.style.box_background, color, alpha);
        if (x < 0)
            width += x, x = 0;
        if (y < 0)
            height += y, y = 0;
        width = RG_MIN(width, gui.screen_width - x);
        height = RG_MIN(height, gui.screen_height - y);
        if (width <= 0 || height <= 0)
            return;
        uint16_t *buffer = get_draw_buffer(width, RG_MIN(height, 16), solid);
        for (int row = 0; row < height; row += 16)
            rg_display_write_rect(x, y + row, width, RG_MIN(height - row, 16), 0, buffer, 0);
        return;
    }

    for (int row = 0; row < height; ++row)
        fill_span(x, y + row, width, color, alpha);
}

void rg_gui_fill_blend(int x_pos, int y_pos, int width, int height, rg_color_t color, int alpha)
{
    fill_block(get_horizontal_position(x_pos, width), get_vertical_position(y_pos, height), width, height, color,
               alpha);
}

void rg_gui_dim_area(int x_pos, int y_pos, int width, int height, int scale)
{
    if (!gui.target.buffer)
        return;

    int x = get_horizontal_position(x_pos, width);
    int y = get_vertical_position(y_pos, height);

    // Clipped to the target window rather than to the screen: this reads pixels back, so it must
    // never step outside the buffer it is reading from.
    int from_x = RG_MAX(x, gui.target.left);
    int from_y = RG_MAX(y, gui.target.top);
    int to_x = RG_MIN(x + width, gui.target.left + gui.target.width);
    int to_y = RG_MIN(y + height, gui.target.top + gui.target.height);

    for (int row = from_y; row < to_y; ++row)
    {
        uint16_t *dst = gui.target.buffer + (row - gui.target.top) * gui.target.stride + (from_x - gui.target.left);
        for (int i = 0; i < to_x - from_x; ++i)
            dst[i] = rg_gui_scale_color(dst[i], scale);
    }
}

void rg_gui_draw_line(int x1, int y1, int x2, int y2, rg_color_t color, int alpha)
{
    // Plain Bresenham. Horizontal runs are handled as spans because that is what most of our
    // lines are (rules, dividers, the splash grid).
    if (y1 == y2)
    {
        fill_span(RG_MIN(x1, x2), y1, abs(x2 - x1) + 1, color, alpha);
        return;
    }

    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    while (true)
    {
        fill_span(x1, y1, 1, color, alpha);
        if (x1 == x2 && y1 == y2)
            break;
        int err2 = err * 2;
        if (err2 >= dy)
            err += dy, x1 += sx;
        if (err2 <= dx)
            err += dx, y1 += sy;
    }
}

void rg_gui_draw_disc(int x_center, int y_center, int radius, rg_color_t color, int alpha)
{
    if (radius <= 0)
        return;

    for (int dy = -radius; dy <= radius; ++dy)
    {
        int dx = (int)(sqrtf((float)(radius * radius - dy * dy)) + 0.5f);
        fill_span(x_center - dx, y_center + dy, dx * 2 + 1, color, alpha);
    }
}

/* How many pixels the given row of a rounded corner is inset by. */
static int corner_inset(int radius, int row)
{
    if (radius <= 0 || row >= radius)
        return 0;
    // Row -1 is asked for when measuring the step above the first row of a corner: it is the row
    // just outside the shape, so it is inset all the way (and sqrtf() must not see a negative).
    if (row < 0)
        return radius;
    float dy = radius - row - 0.5f;
    int dx = (int)(sqrtf((float)(radius * radius) - dy * dy) + 0.5f);
    return RG_MAX(radius - dx, 0);
}

/**
 * A rounded card: the shape every panel, pill and chip in the UI is built from.
 *
 * Corner pixels are simply not drawn rather than filled with a background color, so a card can
 * be laid over a game frame, a photo background or another panel and its corners keep whatever
 * was behind them. That is also why there is no rounded-rect erase: nothing here ever needs to
 * know what it is covering.
 */
static void draw_panel_abs(int x, int y, int width, int height, int radius, rg_color_t fill_color,
                           rg_color_t border_color, int alpha)
{
    if (width <= 0 || height <= 0)
        return;

    radius = RG_MIN(radius, RG_MIN(width, height) / 2);

    bool has_border = border_color != C_NONE && border_color != fill_color;

    for (int row = 0; row < height; ++row)
    {
        int corner_row = (row < radius) ? row : ((row >= height - radius) ? (height - 1 - row) : -1);

        // The straight middle section is one block, the rounded ends go row by row
        if (corner_row < 0)
        {
            int rows = height - radius - row;
            if (has_border)
            {
                fill_block(x, y + row, 1, rows, border_color, alpha);
                fill_block(x + width - 1, y + row, 1, rows, border_color, alpha);
                fill_block(x + 1, y + row, width - 2, rows, fill_color, alpha);
            }
            else
            {
                fill_block(x, y + row, width, rows, fill_color, alpha);
            }
            row += rows - 1;
            continue;
        }

        int inset = corner_inset(radius, corner_row);
        int span_x = x + inset;
        int span_w = width - inset * 2;

        if (span_w <= 0)
            continue;

        if (!has_border)
        {
            fill_span(span_x, y + row, span_w, fill_color, alpha);
            continue;
        }

        // Thickness of the border at the ends of this row: one pixel on the straight parts, and
        // as many as the corner steps in by, so the outline stays closed around the curve.
        int step = corner_inset(radius, corner_row - 1) - inset;
        int edge = RG_MAX(step, 1);

        if (corner_row == 0 || span_w <= edge * 2)
        {
            fill_span(span_x, y + row, span_w, border_color, alpha);
            continue;
        }

        fill_span(span_x, y + row, edge, border_color, alpha);
        fill_span(span_x + span_w - edge, y + row, edge, border_color, alpha);
        fill_span(span_x + edge, y + row, span_w - edge * 2, fill_color, alpha);
    }
}

/* Resolving the position exactly once, here, is what lets the shapes below take (and clip)
 * genuinely negative coordinates: a panel can hang off the top or left edge of the screen without
 * a plain negative y being read as "measured from the bottom". */
void rg_gui_draw_panel(int x_pos, int y_pos, int width, int height, int radius, rg_color_t fill_color,
                       rg_color_t border_color, int alpha)
{
    draw_panel_abs(get_horizontal_position(x_pos, width), get_vertical_position(y_pos, height), width, height, radius,
                   fill_color, border_color, alpha);
}

/**
 * Drop shadow for a card, drawn as a few translucent rings around it.
 *
 * When we can read the target it really is a soft shadow. When we cannot (a dialog over a game)
 * the rings composite against the theme background, which still reads as a shadow on the dark
 * chrome the dialogs use, and it costs nothing when the theme sets "shadow": "none".
 */
void rg_gui_draw_shadow(int x_pos, int y_pos, int width, int height, int radius, int size)
{
    rg_color_t color = gui.palette.shadow;

    if (color == C_NONE || size <= 0)
        return;

    int x = get_horizontal_position(x_pos, width);
    int y = get_vertical_position(y_pos, height);

    for (int i = size; i >= 1; --i)
    {
        // Offset down-right by one so the light appears to come from the top-left
        draw_panel_abs(x - i + 1, y - i + 2, width + i * 2, height + i * 2, radius + i, color, C_NONE, 90 / i);
    }
}

void rg_gui_draw_gradient(int x_pos, int y_pos, int width, int height, rg_color_t from, rg_color_t to,
                          bool horizontal, int alpha)
{
    if (width <= 0 || height <= 0)
        return;

    int x = get_horizontal_position(x_pos, width);
    int y = get_vertical_position(y_pos, height);
    int steps = RG_MAX((horizontal ? width : height) - 1, 1);

    if (horizontal)
    {
        for (int i = 0; i < width; ++i)
            fill_block(x + i, y, 1, height, rg_gui_blend_color(from, to, (i * 255) / steps), alpha);
    }
    else
    {
        for (int i = 0; i < height; ++i)
            fill_span(x, y + i, width, rg_gui_blend_color(from, to, (i * 255) / steps), alpha);
    }
}

void rg_gui_draw_scrollbar(int x_pos, int y_pos, int height, int visible, int total, int offset)
{
    if (total <= visible || height < 8)
        return;

    int x = get_horizontal_position(x_pos, 3);
    int y = get_vertical_position(y_pos, height);
    int thumb = RG_MAX((height * visible) / total, 8);
    int travel = height - thumb;
    int position = (travel * RG_MAX(offset, 0)) / RG_MAX(total - visible, 1);

    // The thumb uses the theme's own "scrollbar" key (which defaults to the accent), so a theme
    // that already chose a scrollbar color keeps it.
    draw_panel_abs(x, y, 3, height, 1, gui.palette.divider, C_NONE, 200);
    draw_panel_abs(x, y + RG_MIN(position, travel), 3, thumb, 1, gui.style.scrollbar, C_NONE, 255);
}

void rg_gui_draw_progress_bar(int x_pos, int y_pos, int width, int height, int percent, rg_color_t fill_color,
                              rg_color_t track_color)
{
    if (width <= 2 || height <= 0)
        return;

    int x = get_horizontal_position(x_pos, width);
    int y = get_vertical_position(y_pos, height);
    int radius = height / 2;
    int filled = (width * RG_MIN(RG_MAX(percent, 0), 100)) / 100;

    draw_panel_abs(x, y, width, height, radius, track_color, C_NONE, 255);
    if (filled > radius * 2)
        draw_panel_abs(x, y, filled, height, radius, fill_color, C_NONE, 255);
    else if (filled > 0)
        fill_block(x, y, filled, height, fill_color, 255);
}

void rg_gui_draw_image(int x_pos, int y_pos, int width, int height, bool resample, const rg_image_t *img)
{
    if (img && resample && (width && height) && (width != img->width || height != img->height))
    {
        rg_image_t *new_img = rg_surface_resize(img, width, height);
        rg_gui_copy_buffer(x_pos, y_pos, width, height, new_img->width * 2, new_img->data, true);
        rg_surface_free(new_img);
    }
    else if (img)
    {
        int draw_width = width ? RG_MIN(width, img->width) : img->width;
        int draw_height = height ? RG_MIN(height, img->height) : img->height;
        rg_gui_copy_buffer(x_pos, y_pos, draw_width, draw_height, img->width * 2, img->data, true);
    }
    else // We fill a rect to show something is missing instead of abort...
    {
        rg_gui_draw_rect(x_pos, y_pos, width, height, 2, gui.style.box_border, gui.style.box_background);
        // rg_gui_draw_text(x_pos + 2, y_pos + 2, width - 4, "No image", gui.style.item_disabled, gui.style.box_background, 0);
    }
}

/**
 * Battery indicator: a rounded cell with a contact tip, filled to the charge level.
 *
 * The fill color is a semantic charge-level indicator (green/amber/red) and is intentionally NOT
 * theme-driven, so a low-battery warning stays universally readable regardless of which color
 * theme is active. Only the chrome around it follows the theme.
 */
void rg_gui_draw_battery_icon(int x_pos, int y_pos, int width, int height)
{
    const rg_gui_palette_t *pal = &gui.palette;
    rg_battery_t battery = rg_input_read_battery();
    int x = get_horizontal_position(x_pos, width);
    int y = get_vertical_position(y_pos, height);
    int level = RG_MIN(RG_MAX((int)battery.level, 0), 100);
    int body_width = RG_MAX(width - 2, 6);
    int tip_height = RG_MAX(height / 2, 3);

    rg_color_t fill = (level > 40) ? C_RGB(76, 210, 128) : ((level > 15) ? C_RGB(250, 190, 64) : C_RGB(244, 82, 82));
    rg_color_t border = battery.charging ? pal->accent : pal->text_dim;

    // Blink the charge (not the shell) when it is nearly empty: the outline stays visible so the
    // icon never looks like it disappeared, but the empty cell is impossible to miss.
    bool blink = !battery.charging && level <= 10 && ((rg_system_timer() / 500000) & 1);

    rg_gui_draw_panel(x, y, body_width, height, 2, rg_gui_scale_color(pal->background, 128), border, 210);
    rg_gui_draw_panel(x + body_width, y + (height - tip_height) / 2, 2, tip_height, 1, border, C_NONE, 255);

    int inner_width = body_width - 4;
    int filled = (inner_width * level) / 100;

    if (!blink && filled > 0)
        rg_gui_draw_panel(x + 2, y + 2, RG_MAX(filled, 1), height - 4, 1, fill, C_NONE, 255);

    if (battery.charging)
    {
        // A little lightning bolt, drawn as a zig-zag so it scales with the icon height
        int cx = x + body_width / 2;
        int top = y + 2, bottom = y + height - 3, middle = y + height / 2;
        for (int i = 0; i < 2; ++i)
        {
            rg_gui_draw_line(cx + 1 + i, top, cx - 1 + i, middle, C_WHITE, 255);
            rg_gui_draw_line(cx - 1 + i, middle, cx + 1 + i, middle, C_WHITE, 255);
            rg_gui_draw_line(cx + 1 + i, middle, cx - 1 + i, bottom, C_WHITE, 255);
        }
    }
}

/* Wi-Fi indicator: three bars, lit according to the signal we actually got from the driver. */
void rg_gui_draw_wifi_icon(int x_pos, int y_pos, int width, int height)
{
    const rg_gui_palette_t *pal = &gui.palette;
    rg_network_t network = rg_network_get_info();
    int x = get_horizontal_position(x_pos, width);
    int y = get_vertical_position(y_pos, height);
    int bars = 3;
    int bar_width = RG_MAX((width - (bars - 1) * 2) / bars, 2);
    int strength = 0;

    if (network.state == RG_NETWORK_CONNECTED)
    {
        // rssi is 0 when the driver does not report it, in which case we show full bars rather
        // than pretending the link is bad.
        strength = (network.rssi == 0 || network.rssi >= -60) ? 3 : (network.rssi >= -70 ? 2 : 1);
    }
    else if (network.state == RG_NETWORK_CONNECTING)
    {
        // Sweep while associating so a failing connection does not look like a connected one
        strength = (int)((rg_system_timer() / 300000) % (bars + 1));
    }

    for (int i = 0; i < bars; ++i)
    {
        int bar_height = (height * (i + 2)) / (bars + 1);
        bool lit = i < strength;
        rg_gui_draw_panel(x + i * (bar_width + 2), y + height - bar_height, bar_width, bar_height, 1,
                          lit ? pal->accent : pal->divider, C_NONE, lit ? 255 : 170);
    }
}

void rg_gui_draw_icons(void)
{
    const rg_gui_palette_t *pal = &gui.palette;
    rg_battery_t battery = rg_input_read_battery();
    rg_network_t network = rg_network_get_info();
    bool show_network = network.state > RG_NETWORK_DISCONNECTED;
    rg_rect_t clock_text = TEXT_RECT("00:00", 0);

    int bar_height = clock_text.height;
    int icon_height = RG_MAX(8, bar_height - 4);
    // Kept at 3 or more: the chip below is drawn three pixels higher, and a negative y would be
    // interpreted as an offset from the bottom of the screen.
    int icon_top = RG_MAX(3, (bar_height - icon_height) / 2);
    int battery_width = icon_height * 2;
    int wifi_width = icon_height;
    int gap = 6;
    int total = 0;

    if (battery.present)
        total += battery_width + 2 + gap;
    if (show_network)
        total += wifi_width + gap;
    if (gui.show_clock)
        total += clock_text.width + gap;

    if (total == 0)
        return;

    total -= gap;

    int right = gui.margins.right;

    // A translucent chip keeps the cluster readable over a theme background image. It is only
    // drawn where we can blend (the launcher); the in-game status bar is already a solid band and
    // a chip there would just be a lighter rectangle inside a black one.
    if (rg_gui_can_blend())
    {
        // The cluster ends one gap short of the right margin (every item reserves a trailing gap),
        // so the chip has to start two gaps out to sit centered on it.
        int chip_height = icon_height + 6;
        rg_gui_draw_panel(-(right + total + gap * 2), icon_top - 3, total + gap * 2, chip_height, chip_height / 2,
                          pal->surface, C_NONE, 150);
    }

    if (battery.present)
    {
        right += battery_width + 2 + gap;
        rg_gui_draw_battery_icon(-right, icon_top, battery_width + 2, icon_height);
    }

    if (show_network)
    {
        right += wifi_width + gap;
        rg_gui_draw_wifi_icon(-right, icon_top, wifi_width, icon_height);
    }

    if (gui.show_clock)
    {
        char buffer[12];
        time_t time_sec = time(NULL);
        struct tm *time = localtime(&time_sec);

        right += clock_text.width + gap;
        sprintf(buffer, "%02d:%02d", time->tm_hour, time->tm_min);
        rg_gui_draw_text(-right, 0, clock_text.width, buffer, pal->text,
                         rg_gui_can_blend() ? C_TRANSPARENT : gui.style.box_background, RG_TEXT_ALIGN_CENTER);
    }
}

void rg_gui_draw_hourglass(void)
{
    rg_display_write_rect(get_horizontal_position(RG_GUI_CENTER, image_hourglass.width),
                          get_vertical_position(RG_GUI_CENTER, image_hourglass.height), image_hourglass.width,
                          image_hourglass.height, image_hourglass.width * 2, (uint16_t *)image_hourglass.pixel_data, 0);
}

void rg_gui_draw_status_bars(void)
{
    size_t max_len = gui.screen_width / 8;
    char header[max_len];
    char footer[max_len];

    const rg_app_t *app = rg_system_get_app();
    rg_stats_t stats = rg_system_get_stats();

    if (!app->initialized || app->isLauncher)
        return;

    snprintf(header, max_len, "SPEED: %d%% (%d %d) / BUSY: %d%%", (int)roundf(stats.speedPercent),
             (int)roundf(stats.totalFPS), (int)app->frameskip, (int)roundf(stats.busyPercent));

    if (app->romPath && strlen(app->romPath) > max_len - 1)
        snprintf(footer, max_len, "...%s", app->romPath + (strlen(app->romPath) - (max_len - 4)));
    else if (app->romPath)
        snprintf(footer, max_len, "%s", app->romPath);
    else
        snprintf(footer, max_len, "Retro-Go %s", app->version);

    // FIXME: Respect gui.margins (draw black background full screen_width, but pad the text if needed)
    const rg_gui_palette_t *pal = &gui.palette;
    int bar_height = TEXT_RECT("ABC", 0).height;

    // Slim bands in the surface color with an accent hairline on the inner edge: the same chrome
    // the launcher and the player use, so the overlay does not look like a different program.
    rg_gui_draw_text(0, RG_GUI_TOP, gui.screen_width, header, pal->text, pal->surface, 0);
    rg_gui_fill_blend(0, bar_height, gui.screen_width, 1, pal->accent, 130);
    rg_gui_draw_text(0, RG_GUI_BOTTOM, gui.screen_width, footer, pal->text_dim, pal->surface, 0);
    rg_gui_fill_blend(0, gui.screen_height - bar_height - 1, gui.screen_width, 1, pal->divider, 255);

    rg_gui_draw_icons();
}

static size_t get_dialog_items_count(const rg_gui_option_t *options)
{
    if (!options)
        return 0;

    const rg_gui_option_t *opt = options;
    while (opt->arg || opt->label || opt->value || opt->flags || opt->update_cb)
        opt++;
    return opt - options;
}

/* A row whose label is nothing but dashes is the idiom the option arrays use for a separator.
 * We recognize both that and the explicit flag, and draw a hairline instead of the dashes. */
static bool is_separator_row(const rg_gui_option_t *option)
{
    if ((option->flags & RG_DIALOG_FLAG_TYPE_MASK) == (RG_DIALOG_FLAG_SEPARATOR & RG_DIALOG_FLAG_TYPE_MASK))
        return true;

    const char *label = option->label;
    if (!label || option->value || strlen(label) < 3)
        return false;
    while (*label == '-')
        label++;
    return *label == 0;
}

rg_rect_t rg_gui_draw_dialog(const char *title, const rg_gui_option_t *options, size_t options_count,
                             int sel) // const rg_rect_t *rect,
{
    RG_ASSERT_ARG(options || options_count == 0);

    const rg_gui_palette_t *pal = &gui.palette;
    const int sep_width = TEXT_RECT(": ", 0).width;
    const int font_height = gui.font_height;
    const int text_height = font_height + 2; // rg_gui_draw_text pads a pixel above and below
    const int max_box_width = 0.86f * gui.screen_width;
    const int max_box_height = 0.86f * gui.screen_height;
    const int box_padding = 7;
    const int box_radius = 7;
    const int row_padding_x = 7;
    const int title_height = title ? text_height + 6 : 0;
    const int title_gap = title ? 5 : 0;
    const int max_inner_width = max_box_width - sep_width - (row_padding_x + box_padding) * 2;

    int box_x, box_y;
    int box_width = box_padding * 2;
    int box_height = box_padding * 2 + title_height + title_gap;
    int inner_width = TEXT_RECT(title, 0).width;
    int col1_width = -1;
    int col2_width = -1;
    uint8_t row_height[options_count];

    // FIXME: The information built in this loop should be cached between calls to rg_gui_draw_dialog...
    // It doesn't matter for most dialogs but the file picker with 500+ files wastes a LOT of time here.
    for (size_t i = 0; i < options_count; i++)
    {
        if ((options[i].flags & RG_DIALOG_FLAG_MODE_MASK) == RG_DIALOG_FLAG_HIDDEN)
        {
            row_height[i] = 0;
            continue;
        }

        rg_rect_t label = TEXT_RECT(options[i].label, max_inner_width);
        rg_rect_t value = {0};

        inner_width = RG_MAX(inner_width, label.width);

        if (options[i].value)
        {
            value = TEXT_RECT(options[i].value, max_inner_width - label.width);
            col1_width = RG_MAX(col1_width, label.width);
            col2_width = RG_MAX(col2_width, value.width);
        }

        row_height[i] = RG_MAX(label.height, value.height);
        box_height += row_height[i];
    }

    col1_width = RG_MIN(col1_width, max_box_width);
    col2_width = RG_MIN(col2_width, max_box_width);

    if (col2_width >= 0)
        inner_width = RG_MAX(inner_width, col1_width + col2_width + sep_width);

    inner_width = RG_MIN(inner_width, max_box_width);
    col2_width = inner_width - col1_width - sep_width;
    box_width += inner_width + row_padding_x * 2;
    box_height = RG_MIN(box_height, max_box_height);
    box_x = (gui.screen_width - box_width) / 2;
    box_y = (gui.screen_height - box_height) / 2;

    // Everything below is painted into a scratch buffer and sent to the panel as a single transfer
    // (see begin_offscreen). Drawing a card straight to the LCD means the user watches it being
    // built row by row on every keypress, which is exactly the flicker this avoids.
    //
    // The shadow needs to know what is behind the card, so it is only drawn when there is a
    // backdrop to composite over; otherwise the card sits on the flat plate begin_offscreen makes.
    int shadow_size = gui.backdrop ? 3 : 0;
    int margin = shadow_size ? shadow_size + 2 : 0;
    rg_rect_t overlay = {box_x - margin, box_y - margin, box_width + margin * 2, box_height + margin * 2 + 1};
    rg_gui_target_t saved_target;

    // Union with the previous overlay, so a dialog that just got narrower does not leave a strip of
    // its old self on screen.
    if (gui.last_overlay.width > 0)
    {
        int left = RG_MIN(overlay.left, gui.last_overlay.left);
        int top = RG_MIN(overlay.top, gui.last_overlay.top);
        int right = RG_MAX(overlay.left + overlay.width, gui.last_overlay.left + gui.last_overlay.width);
        int bottom = RG_MAX(overlay.top + overlay.height, gui.last_overlay.top + gui.last_overlay.height);
        overlay = (rg_rect_t){left, top, right - left, bottom - top};
    }

    bool composited = begin_offscreen(overlay, C_NONE, &saved_target);

    if (composited)
        gui.last_overlay = overlay;

    if (shadow_size)
        rg_gui_draw_shadow(box_x, box_y, box_width, box_height, box_radius, shadow_size);
    rg_gui_draw_panel(box_x, box_y, box_width, box_height, box_radius, gui.style.box_background, pal->border,
                      255);
    rg_gui_fill_blend(box_x + box_radius, box_y + 1, box_width - box_radius * 2, 1,
                      rg_gui_blend_color(pal->border, C_WHITE, 70), 255);

    int x = box_x + box_padding;
    int y = box_y + box_padding;

    if (title)
    {
        // Header chip: inset from the card so its own rounded corners never fight the card's, with
        // an accent bar on the leading edge to anchor it.
        int chip_width = inner_width + row_padding_x * 2;
        rg_gui_draw_panel(x, y, chip_width, title_height, 4, pal->surface_alt, C_NONE, 255);
        rg_gui_draw_panel(x, y + 2, 3, title_height - 4, 1, pal->accent, C_NONE, 255);
        rg_gui_draw_text(x + 7, y + (title_height - text_height) / 2, chip_width - 14, title, gui.style.box_header,
                         pal->surface_alt, RG_TEXT_ALIGN_CENTER);
        y += title_height + title_gap;
    }

    int list_top = y;
    int list_height = (box_y + box_height - box_padding) - list_top;
    int list_end_i = 0;

    // Menus scroll one row at a time: the selection walks to the edge of the card and the list then
    // follows it by a single row. Paging (which is what this used to do) is right for a game list
    // you skim, but wrong for a menu, where jumping a whole page loses your place.
    //
    // min_top is the highest row that still leaves the selection visible; the remembered scroll
    // position is simply clamped into [min_top, sel], which moves it by exactly one row when the
    // selection steps off either edge.
    int min_top = sel;
    for (int i = sel, used = 0; i >= 0; --i)
    {
        used += row_height[i];
        if (used > list_height)
            break;
        min_top = i;
    }

    int list_top_i = RG_MIN(RG_MAX(gui.dialog_top, min_top), RG_MAX(sel, 0));
    gui.dialog_top = list_top_i;

    for (int i = list_top_i; i < options_count; i++)
    {
        int option_type = options[i].flags & RG_DIALOG_FLAG_TYPE_MASK;
        int option_mode = options[i].flags & RG_DIALOG_FLAG_MODE_MASK;
        int row_width = inner_width + row_padding_x * 2;
        int height = row_height[i];
        rg_color_t color;

        if (option_mode == RG_DIALOG_FLAG_NORMAL)
            color = gui.style.item_standard;
        else if (option_type == (RG_DIALOG_FLAG_MESSAGE & RG_DIALOG_FLAG_TYPE_MASK))
            color = gui.style.item_message;
        else
            color = gui.style.item_disabled;

        // The first row of a page is always drawn even if it is taller than the space left: a long
        // message is better clipped at the card edge than replaced by an empty card.
        if (i > list_top_i && y + height > box_y + box_height - box_padding)
            break;

        list_end_i = i;

        if (option_mode == RG_DIALOG_FLAG_HIDDEN)
            continue;

        bool highlight = option_mode != RG_DIALOG_FLAG_SKIP && i == sel;
        rg_color_t fg = highlight ? pal->highlight : color;
        rg_color_t bg = highlight ? pal->accent_dim : gui.style.box_background;
        rg_color_t value_fg = highlight ? pal->highlight : gui.style.item_value;

        if (highlight)
        {
            // Selection pill plus a leading accent bar. The text below is drawn with the pill color
            // as its background, so the two always match seamlessly even on the LCD path where we
            // cannot read back what is underneath.
            rg_gui_draw_panel(x, y, row_width, height, RG_MIN(4, height / 2), pal->accent_dim, C_NONE, 255);
            rg_gui_draw_panel(x, y + 1, 3, height - 2, 1, pal->accent, C_NONE, 255);
        }

        if (is_separator_row(&options[i]))
        {
            rg_gui_fill_blend(x + row_padding_x, y + height / 2, inner_width, 1, pal->divider, 255);
        }
        else if (options[i].value)
        {
            int text_x = x + row_padding_x;
            rg_gui_draw_text(text_x, y, col1_width, options[i].label, fg, bg, 0);
            rg_gui_draw_text(text_x + col1_width, y, sep_width, "  ", fg, bg, 0);
            // Values start at a fixed column, which is what lines them up; right-aligning them on top
            // of that only pushed each one out to the far edge, leaving a gap after its label. On rows
            // that are information rather than a setting (the About screen's version, date, target and
            // website) that reads as text that has been shoved to the right.
            int value_height = rg_gui_draw_text(text_x + col1_width + sep_width, y, col2_width,
                                                options[i].value, value_fg, bg, RG_TEXT_MULTILINE)
                                   .height;
            if ((value_height / text_height) >= 2) // Multiline value, must fill sep and label
                rg_gui_fill_blend(text_x, y + text_height, inner_width - col2_width, value_height - text_height, bg,
                                  255);
        }
        else
        {
            uint32_t flags = RG_TEXT_MULTILINE;
            if (options[i].flags & RG_DIALOG_FLAG_ALIGN_CENTER)
                flags |= RG_TEXT_ALIGN_CENTER;
            else if (option_type == (RG_DIALOG_FLAG_MESSAGE & RG_DIALOG_FLAG_TYPE_MASK))
                flags |= RG_TEXT_ALIGN_CENTER;
            rg_gui_draw_text(x + row_padding_x, y, inner_width, options[i].label, fg, bg, flags);
        }

        y += height;
    }

    // The scrollbar lives in the card's right-hand padding, so a long list never loses text width
    // to it and short lists show nothing at all.
    rg_gui_draw_scrollbar(box_x + box_width - box_padding + 1, list_top, list_height, list_end_i - list_top_i + 1,
                          options_count, list_top_i);

    if (composited)
        end_offscreen(&saved_target);

    return (rg_rect_t){box_x, box_y, box_width, box_height};
}

static rg_rect_t draw_message_vargs(int flags, const char *format, va_list va)
{
    RG_ASSERT_ARG(format);

    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, va);
    const rg_gui_option_t options[] = {
        {0, buffer, NULL, RG_DIALOG_FLAG_MESSAGE | flags, NULL},
        RG_DIALOG_END,
    };
    // FIXME: Should rg_display_force_redraw() be called? Before? After? Both?
    return rg_gui_draw_dialog(NULL, options, 1, 0);
}

rg_rect_t rg_gui_draw_message_flags(int flags, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    rg_rect_t rect = draw_message_vargs(flags, format, va);
    va_end(va);
    return rect;
}

rg_rect_t rg_gui_draw_message(const char *format, ...) // const rg_rect_t *rect,
{
    va_list va;
    va_start(va, format);
    rg_rect_t rect = draw_message_vargs(0, format, va);
    va_end(va);
    return rect;
}

intptr_t rg_gui_dialog(const char *title, const rg_gui_option_t *options_const, int selected_index)
{
    rg_gui_option_t *options = (rg_gui_option_t *)options_const;
    size_t options_count = get_dialog_items_count(options_const);

    // In many cases we must create a copy of the options array because it can be mutated by the callbacks
    // (typically option->value and option->flags). No callback in the array = no way of it being mutable.
    // The entire text_buffer system is very brittle and prone to corruption. We get away with it for now
    // because most values are less than our assumed 32 bytes...
    size_t shadow_options_count = 0;
    size_t shadow_text_buffer_size = 0;
    for (size_t i = 0; i < options_count; i++)
    {
        if (options_const[i].update_cb)
            shadow_options_count = options_count;
        // if (options_const[i].value)
        //     shadow_text_buffer_size += strlen(options_const[i].value) + 1;
    }
    rg_gui_option_t shadow_options[shadow_options_count + 1];
    char *shadow_text_buffer = NULL;
    if (shadow_options_count > 0)
    {
        options = memcpy(shadow_options, options_const, sizeof(shadow_options));
        shadow_text_buffer_size = RG_MAX(options_count * 32, 1024);
        shadow_text_buffer = malloc(shadow_text_buffer_size);
        char *text_buffer_ptr = shadow_text_buffer;
        for (size_t i = 0; i < shadow_options_count; i++)
        {
            rg_gui_option_t *option = &shadow_options[i];
            if (!text_buffer_ptr || !option->value || !option->update_cb)
                continue;
            option->value = strcpy(text_buffer_ptr, option->value);
            option->update_cb(option, RG_DIALOG_INIT);
            text_buffer_ptr += RG_MAX(strlen(text_buffer_ptr), 31) + 1;
        }
    }

    if (selected_index < 0)
        selected_index += options_count;

    rg_gui_event_t event = RG_DIALOG_VOID;
    uint32_t joystick = 0, joystick_old;
    uint64_t joystick_last = 0;
    bool redraw = false;
    int sel = RG_MIN(RG_MAX(0, selected_index), options_count - 1);
    int sel_old = -1;

    // Fresh dialog: start at the top of the list, and forget the window the previous overlay used
    gui.dialog_top = 0;
    gui.last_overlay = (rg_rect_t){0};

    rg_gui_draw_status_bars();
    rg_gui_draw_dialog(title, options, options_count, sel);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_task_delay(80);

    while (event != RG_DIALOG_SELECT && event != RG_DIALOG_CANCEL)
    {
        // TO DO: Add acceleration!
        joystick_old = ((rg_system_timer() - joystick_last) > 300000) ? 0 : joystick;
        joystick = rg_input_read_gamepad();
        event = RG_DIALOG_VOID;

        if (joystick ^ joystick_old)
        {
            bool active_selection = options_count && options[sel].flags == RG_DIALOG_FLAG_NORMAL;
            rg_gui_callback_t callback = active_selection ? options[sel].update_cb : NULL;

            if (joystick & RG_KEY_UP)
            {
                if (--sel < 0)
                    sel = options_count - 1;
            }
            else if (joystick & RG_KEY_DOWN)
            {
                if (++sel > options_count - 1)
                    sel = 0;
            }
            else if (joystick & (RG_KEY_B | RG_KEY_OPTION | RG_KEY_MENU))
            {
                event = RG_DIALOG_CANCEL;
            }
            else if (joystick & RG_KEY_LEFT && callback)
            {
                event = callback(&options[sel], RG_DIALOG_PREV);
                redraw = true;
            }
            else if (joystick & RG_KEY_RIGHT && callback)
            {
                event = callback(&options[sel], RG_DIALOG_NEXT);
                redraw = true;
            }
            else if (joystick & RG_KEY_A && callback)
            {
                event = callback(&options[sel], RG_DIALOG_ENTER);
                redraw = true;
            }
            else if (joystick & RG_KEY_A && active_selection)
            {
                event = RG_DIALOG_SELECT;
            }

            joystick_last = rg_system_timer();
        }

        if (sel_old != sel)
        {
            for (size_t i = 0; i < options_count; ++i)
            {
                // If the item is selectable, we stop here
                if (options[sel].flags == RG_DIALOG_FLAG_NORMAL)
                    break;
                if (options[sel].flags == RG_DIALOG_FLAG_DISABLED)
                    break;

                // Otherwise move to the next
                sel += (joystick == RG_KEY_UP) ? -1 : 1;

                if (sel < 0)
                    sel = options_count - 1;

                if (sel >= options_count)
                    sel = 0;
            }
            if (sel_old != -1 && options[sel_old].update_cb)
                options[sel_old].update_cb(&options[sel_old], RG_DIALOG_FOCUS_LOST);
            if (options[sel].update_cb)
                options[sel].update_cb(&options[sel], RG_DIALOG_FOCUS_GAINED);
            redraw = true;
            sel_old = sel;
        }

        if (event == RG_DIALOG_REDRAW || event == RG_DIALOG_UPDATE)
        {
            for (size_t i = 0; i < options_count; i++)
            {
                if (options[i].update_cb)
                    options[i].update_cb(&options[i], RG_DIALOG_UPDATE);
            }
            if (event == RG_DIALOG_REDRAW)
            {
                // The app has just repainted the whole screen, so the region our overlay owned is
                // gone: there is nothing left to clean up, and the card is about to be drawn again.
                rg_display_force_redraw();
                gui.last_overlay = (rg_rect_t){0};
                rg_gui_draw_status_bars();
            }
            redraw = true;
        }

        if (redraw)
        {
            rg_gui_draw_dialog(title, options, options_count, sel);
            redraw = false;
        }

        rg_task_delay(20);
        rg_system_tick(0);
    }

    rg_input_wait_for_key(joystick, false, 1000);
    rg_display_force_redraw();
    gui.last_overlay = (rg_rect_t){0};
    // free(shadow_options);
    free(shadow_text_buffer);

    if (event == RG_DIALOG_CANCEL || sel < 0)
        return RG_DIALOG_CANCELLED;

    return options[sel].arg;
}

bool rg_gui_confirm(const char *title, const char *message, bool default_yes)
{
    const rg_gui_option_t options[] = {
        {0, message,  NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "",       NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {1, _("Yes"), NULL, RG_DIALOG_FLAG_NORMAL,  NULL},
        {0, _("No"),  NULL, RG_DIALOG_FLAG_NORMAL,  NULL},
        RG_DIALOG_END,
    };
    return rg_gui_dialog(title, message ? options : options + 1, default_yes ? -2 : -1) == 1;
}

void rg_gui_alert(const char *title, const char *message)
{
    const rg_gui_option_t options[] = {
        {0, message, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "",      NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {1, _("OK"), NULL, RG_DIALOG_FLAG_NORMAL,  NULL},
        RG_DIALOG_END,
    };
    rg_gui_dialog(title, message ? options : options + 1, -1);
}

typedef struct
{
    rg_gui_option_t *options;
    size_t count;
    rg_bucket_t *filenames;
    bool (*validator)(const char *path);
} file_picker_opts_t;

static int file_picker_cb(const rg_scandir_t *entry, void *arg)
{
    file_picker_opts_t *f = arg;
    if (f->validator && !(f->validator)(entry->path))
        return RG_SCANDIR_SKIP;
    rg_gui_option_t *options = realloc(f->options, (f->count + 2) * sizeof(rg_gui_option_t));
    if (!options)
        return RG_SCANDIR_STOP;
    char *name = rg_bucket_insert(f->filenames, entry->basename, strlen(entry->basename) + 1);
    f->options = options;
    f->options[f->count++] = (rg_gui_option_t){(intptr_t)name, name, NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    return RG_SCANDIR_CONTINUE;
}

char *rg_gui_file_picker(const char *title, const char *path, bool (*validator)(const char *path), bool browse_tree,
                         bool none_option)
{
    file_picker_opts_t options = {
        .options = calloc(8, sizeof(rg_gui_option_t)),
        .count = 0,
        .filenames = rg_bucket_create(4096),
        .validator = validator,
    };
    char *filepath = NULL;

    if (!title)
        title = _("Select file");

    if (none_option)
        options.options[options.count++] = (rg_gui_option_t){0, _("<None>"), NULL, RG_DIALOG_FLAG_NORMAL, NULL};

    // if (browse_tree)
    //     options.options[options.count++] = (rg_gui_option_t){0, "...", NULL, RG_DIALOG_FLAG_NORMAL, NULL};

    if (!rg_storage_scandir(path, file_picker_cb, &options, 0) || options.count < 1)
    {
        rg_gui_alert(title, _("Folder is empty."));
        goto cleanup;
    }
    options.options[options.count] = (rg_gui_option_t)RG_DIALOG_END;

    char *filename = (char *)rg_gui_dialog(title, options.options, 0);
    if (filename != (void *)RG_DIALOG_CANCELLED)
    {
        char buffer[RG_PATH_MAX] = "";
        if (filename)
            snprintf(buffer, RG_PATH_MAX, "%s/%s", path, filename);
        filepath = strdup(buffer);
        // if (browse_tree && rg_storage_stat(filepath).is_dir)
        // {
        // }
    }

cleanup:
    rg_bucket_free(options.filenames);
    free(options.options);
    return filepath;
}

void rg_gui_draw_input_screen(const char *title, const char *message, const char *input_buffer,
                              const rg_keyboard_layout_t *current_layout, int cursor_pos, bool partial_redraw)
{
    const rg_gui_palette_t *pal = &gui.palette;
    const int text_height = gui.font_height + 2;
    const int key_width = gui.screen_width / 10 - 4;
    const int key_height = RG_MAX(text_height + 8, 18);
    const int keyboard_width = current_layout->columns * key_width;
    const int keyboard_height = current_layout->rows * key_height;
    const int keyboard_x = (gui.screen_width - keyboard_width) / 2;
    const int keyboard_y = gui.screen_height - keyboard_height - text_height - 10;
    const int input_box_height = text_height + 10;
    const int input_box_y = keyboard_y - input_box_height - 10;
    char text_buffer[200];

    if (!input_buffer)
        input_buffer = "";

    if (!partial_redraw)
    {
        rg_gui_draw_rect(0, 0, gui.screen_width, gui.screen_height, 0, C_NONE, gui.style.box_background);

        // Header chip, the same one the dialogs use, so this screen belongs to the same UI
        if (title)
        {
            int chip_height = text_height + 6;
            rg_gui_draw_panel(keyboard_x, 8, keyboard_width, chip_height, 4, pal->surface_alt, C_NONE, 255);
            rg_gui_draw_panel(keyboard_x, 10, 3, chip_height - 4, 1, pal->accent, C_NONE, 255);
            rg_gui_draw_text(keyboard_x + 7, 8 + (chip_height - text_height) / 2, keyboard_width - 14, title,
                             gui.style.box_header, pal->surface_alt, RG_TEXT_ALIGN_CENTER);
        }

        if (message)
            rg_gui_draw_text(0, title ? text_height + 20 : 10, gui.screen_width, message, gui.style.item_message,
                             gui.style.box_background, RG_TEXT_ALIGN_CENTER);

        snprintf(text_buffer, sizeof(text_buffer), "A Type   B Erase   SELECT %s   START OK   MENU Cancel",
                 current_layout->label);
        rg_gui_draw_text(0, gui.screen_height - text_height - 3, gui.screen_width, text_buffer, pal->text_dim,
                         gui.style.box_background, RG_TEXT_ALIGN_CENTER);
    }

    // Input field: dark, with an accent outline, because it is the one thing on screen that is
    // being edited right now. Composited for the same reason as the keypad below.
    rg_gui_target_t saved_target;
    bool composited = begin_offscreen((rg_rect_t){keyboard_x, input_box_y, keyboard_width, input_box_height},
                                      gui.style.box_background, &saved_target);

    rg_gui_draw_panel(keyboard_x, input_box_y, keyboard_width, input_box_height, 4, pal->surface, pal->accent, 255);

    // Draw input buffer text and blinking cursor
    // static uint32_t blink_timer = 0;
    static bool show_cursor = true;
    snprintf(text_buffer, sizeof(text_buffer), "%s%s", input_buffer, show_cursor ? "_" : " ");
    rg_gui_draw_text(keyboard_x + 6, input_box_y + (input_box_height - text_height) / 2, keyboard_width - 12,
                     text_buffer, pal->text, pal->surface, 0);

    if (composited)
        end_offscreen(&saved_target);

    rg_gui_draw_virtual_keyboard(keyboard_x, keyboard_y, current_layout, cursor_pos, partial_redraw);
}

void rg_gui_draw_virtual_keyboard(int x_pos, int y_pos, const rg_keyboard_layout_t *current_layout, int cursor_pos,
                                  bool partial_redraw)
{
    const rg_gui_palette_t *pal = &gui.palette;
    const int text_height = gui.font_height + 2;
    const int key_width = gui.screen_width / 10 - 4;
    const int key_height = RG_MAX(text_height + 8, 18);
    const int keyboard_width = current_layout->columns * key_width;
    const int keyboard_height = current_layout->rows * key_height;
    const int keyboard_x = get_horizontal_position(x_pos, keyboard_width);
    const int keyboard_y = get_vertical_position(y_pos, keyboard_height);
    const char *layout_ptr = current_layout->layout;

    // The whole keypad is composited and sent in one transfer: it is redrawn on every keypress, and
    // forty little panels going out one at a time is very visible. It seeds from the screen's own
    // background rather than the backdrop, because this screen painted that background itself.
    rg_gui_target_t saved_target;
    bool composited = begin_offscreen(
        (rg_rect_t){keyboard_x - 4, keyboard_y - 4, keyboard_width + 8, keyboard_height + 8},
        gui.style.box_background, &saved_target);

    if (composited || !partial_redraw)
        rg_gui_draw_panel(keyboard_x - 3, keyboard_y - 3, keyboard_width + 6, keyboard_height + 6, 5, pal->surface,
                          pal->divider, 255);

    for (int row = 0; row < current_layout->rows; row++)
    {
        for (int col = 0; col < current_layout->columns; col++)
        {
            int key_idx = row * current_layout->columns + col;
            int x = keyboard_x + col * key_width;
            int y = keyboard_y + row * key_height;

            bool is_selected = (cursor_pos == key_idx);
            rg_color_t bg_color = is_selected ? pal->accent : pal->surface_alt;
            rg_color_t fg_color = is_selected ? gui.style.box_background : pal->text;

            rg_gui_draw_panel(x + 1, y + 1, key_width - 2, key_height - 2, 2, bg_color,
                              is_selected ? pal->highlight : C_NONE, 255);

            // Draw key character
            char key_str[5] = {0, 0, 0, 0, 0};
            int key = rg_utf8_decode(&layout_ptr);
            if (key == ' ')
                strcpy(key_str, "SP");
            else
                rg_utf8_encode(key_str, key);

            rg_gui_draw_text(x + 3, y + (key_height - text_height) / 2, key_width - 6, key_str, fg_color, bg_color,
                             RG_TEXT_ALIGN_CENTER);
        }
    }

    if (composited)
        end_offscreen(&saved_target);
}

static const rg_keyboard_layout_t keyboard_layouts[] = {
    // Lowercase letters
    {
     .layout = "1234567890"
                  "qwertyuiop"
                  "asdfghjkl "
                  "zxcvbnm.,?", .columns = 10,
     .rows = 4,
     .label = "ABC",
     },
    // Uppercase letters
    {
     .layout = "1234567890"
                  "QWERTYUIOP"
                  "ASDFGHJKL "
                  "ZXCVBNM.,?",                         .columns = 10,
     .rows = 4,
     .label = "abc",
     },
    // Symbols
    {
     .layout = "!@#$%^&*()"
                  "[]{}|\\:;\"'"
                  "<>?/+=_-~ "
                  "1234567890",                 .columns = 10,
     .rows = 4,
     .label = "!@#",
     }
};

// TODO: Abstract all the redundant/similar code between rg_gui_input_str and rg_gui_input_char

int rg_gui_input_char(const rg_keyboard_layout_t *map)
{
    if (!map)
        map = &keyboard_layouts[0];

    int cursor = -1;
    int count = map->columns * map->rows;

    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);

    while (1)
    {
        uint32_t joystick = rg_input_read_gamepad();
        int prev_cursor = cursor;

        if (joystick & RG_KEY_A)
            return map->layout[cursor];
        if (joystick & RG_KEY_B)
            break;

        if (joystick & RG_KEY_LEFT)
            cursor--;
        if (joystick & RG_KEY_RIGHT)
            cursor++;
        if (joystick & RG_KEY_UP)
            cursor -= map->columns;
        if (joystick & RG_KEY_DOWN)
            cursor += map->columns;

        if (cursor > count - 1)
            cursor = prev_cursor;
        else if (cursor < 0)
            cursor = prev_cursor;

        cursor = RG_MIN(RG_MAX(cursor, 0), count - 1);

        if (cursor != prev_cursor)
            rg_gui_draw_virtual_keyboard(RG_GUI_CENTER, RG_GUI_BOTTOM, map, cursor, false);

        rg_input_wait_for_key(RG_KEY_ALL, false, 500);
        rg_input_wait_for_key(RG_KEY_ANY, true, 500);

        rg_system_tick(0);
    }

    return -1;
}

char *rg_gui_input_str(const char *title, const char *message, const char *default_value)
{
    // Virtual keyboard implementation for Wi-Fi credential input
    char input_buffer[128] = {0};
    if (default_value)
        strncpy(input_buffer, default_value, sizeof(input_buffer) - 1);

    int cursor_pos = 0; // Position in keyboard grid
    int layout_idx = 0; // Current keyboard layout
    int input_length = strlen(input_buffer);
    bool cancelled = false;

    const rg_keyboard_layout_t *current_layout = &keyboard_layouts[layout_idx];

    // Follow the same pattern as rg_gui_dialog
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_task_delay(80);

    uint32_t joystick = 0, joystick_old;
    uint64_t joystick_last = 0;
    bool redraw = true;
    int redraws = 0;

    while (true)
    {
        // Handle input similar to rg_gui_dialog
        joystick_old = ((rg_system_timer() - joystick_last) > 300000) ? 0 : joystick;
        joystick = rg_input_read_gamepad();

        if (joystick ^ joystick_old)
        {
            if (joystick & RG_KEY_LEFT)
            {
                cursor_pos--;
                if (cursor_pos < 0)
                    cursor_pos = (current_layout->columns * current_layout->rows) - 1;
                redraw = true;
            }
            else if (joystick & RG_KEY_RIGHT)
            {
                cursor_pos++;
                if (cursor_pos >= current_layout->columns * current_layout->rows)
                    cursor_pos = 0;
                redraw = true;
            }
            else if (joystick & RG_KEY_UP)
            {
                cursor_pos -= current_layout->columns;
                if (cursor_pos < 0)
                    cursor_pos += current_layout->columns * current_layout->rows;
                redraw = true;
            }
            else if (joystick & RG_KEY_DOWN)
            {
                cursor_pos += current_layout->columns;
                if (cursor_pos >= current_layout->columns * current_layout->rows)
                    cursor_pos -= current_layout->columns * current_layout->rows;
                redraw = true;
            }
            else if (joystick & RG_KEY_A)
            {
                if (input_length < sizeof(input_buffer) - 4)
                {
                    const char *layout_ptr = current_layout->layout;
                    int key = 0;
                    for (int i = 0; i <= cursor_pos; ++i)
                        key = rg_utf8_decode(&layout_ptr);
                    input_length += rg_utf8_encode(&input_buffer[input_length], key);
                    input_buffer[input_length] = '\0';
                    redraw = true;
                }
            }
            else if (joystick & RG_KEY_B)
            {
                // Backspace
                while (input_length > 0)
                {
                    // Rewind until we find a valid codepoint
                    const char *ptr = &input_buffer[--input_length];
                    if (rg_utf8_decode(&ptr) != -1)
                        break;
                }
                input_buffer[input_length] = '\0';
                redraw = true;
            }
            else if (joystick & RG_KEY_SELECT)
            {
                // Toggle between layouts (Shift/Symbols)
                layout_idx = (layout_idx + 1) % RG_COUNT(keyboard_layouts);
                current_layout = &keyboard_layouts[layout_idx];
                cursor_pos = 0;
                redraw = true;
                redraws = 0;
            }
            else if (joystick & RG_KEY_START)
            {
                // OK/Enter - confirm input
                break;
            }
            else if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
            {
                // Cancel
                cancelled = true;
                break;
            }

            joystick_last = rg_system_timer();
        }

        if (redraw)
        {
            rg_gui_draw_input_screen(title, message, input_buffer, current_layout, cursor_pos, redraws++ > 0);
            redraw = false;
        }

        rg_task_delay(20);
        rg_system_tick(0);
    }

    rg_input_wait_for_key(joystick, false, 1000);
    rg_display_force_redraw();

    if (cancelled)
        return NULL;

    return input_length > 0 ? strdup(input_buffer) : NULL;
}

static rg_gui_event_t volume_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int level = rg_audio_get_volume();
    int prev_level = level;

    if (event == RG_DIALOG_PREV)
        level -= 5;
    if (event == RG_DIALOG_NEXT)
        level += 5;

    level -= (level % 5);

    if (level != prev_level)
        rg_audio_set_volume(level);

    sprintf(option->value, "%d%%", rg_audio_get_volume());

    return RG_DIALOG_VOID;
}

static rg_gui_event_t brightness_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int level = rg_display_get_backlight();
    int prev_level = level;

    if (event == RG_DIALOG_PREV)
        level -= 10;
    if (event == RG_DIALOG_NEXT)
        level += 10;

    level -= (level % 10);

    if (level != prev_level)
        rg_display_set_backlight(RG_MAX(level, 1));

    sprintf(option->value, "%d%%", rg_display_get_backlight());

    return RG_DIALOG_VOID;
}

static rg_gui_event_t audio_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    size_t count = 0;
    const rg_audio_sink_t *sinks = rg_audio_get_sinks(&count);
    const rg_audio_sink_t *ssink = rg_audio_get_sink();
    // Hide dummy unless it's the only one or we're a debug build
    int min = rg_system_get_app()->isRelease ? (1 % count) : (0);
    int max = count - 1;
    int sink = 0;

    // If there's no choice to be made we can just hide the entry
    if (min == max)
    {
        option->flags |= RG_DIALOG_FLAG_HIDDEN;
        return RG_DIALOG_VOID;
    }

    for (int i = 0; i < count; ++i)
        if (sinks[i].driver == ssink->driver && sinks[i].device == ssink->device)
            sink = i;

    int prev_sink = sink;

    if (event == RG_DIALOG_PREV && --sink < min)
        sink = max;
    if (event == RG_DIALOG_NEXT && ++sink > max)
        sink = min;

    if (sink != prev_sink)
        rg_audio_set_sink(sinks[sink].driver->name, sinks[sink].device);

    // Ask the audio layer rather than using sinks[sink].name directly: an automatic sink only
    // becomes meaningful once the hardware has resolved it to a speaker or headphones.
    rg_audio_get_sink_label(option->value, 32);

    return RG_DIALOG_VOID;
}

static rg_gui_event_t filter_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int max = RG_DISPLAY_FILTER_COUNT - 1;
    int mode = rg_display_get_filter();
    int prev_mode = mode;

    if (event == RG_DIALOG_PREV && --mode < 0)
        mode = max;
    if (event == RG_DIALOG_NEXT && ++mode > max)
        mode = 0;

    if (mode != prev_mode)
    {
        rg_display_set_filter(mode);
        return RG_DIALOG_REDRAW;
    }

    if (mode == RG_DISPLAY_FILTER_OFF)
        strcpy(option->value, _("Off"));
    if (mode == RG_DISPLAY_FILTER_HORIZ)
        strcpy(option->value, _("Horiz"));
    if (mode == RG_DISPLAY_FILTER_VERT)
        strcpy(option->value, _("Vert"));
    if (mode == RG_DISPLAY_FILTER_BOTH)
        strcpy(option->value, _("Both"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t scaling_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int max = RG_DISPLAY_SCALING_COUNT - 1;
    int mode = rg_display_get_scaling();
    int prev_mode = mode;

    if (event == RG_DIALOG_PREV && --mode < 0)
        mode = max; // 0;
    if (event == RG_DIALOG_NEXT && ++mode > max)
        mode = 0; // max;

    if (mode != prev_mode)
    {
        rg_display_set_scaling(mode);
        return RG_DIALOG_REDRAW;
    }

    if (mode == RG_DISPLAY_SCALING_OFF)
        strcpy(option->value, _("Off"));
    else if (mode == RG_DISPLAY_SCALING_FIT)
        strcpy(option->value, _("Fit"));
    else if (mode == RG_DISPLAY_SCALING_FULL)
        strcpy(option->value, _("Full"));
    else if (mode == RG_DISPLAY_SCALING_ZOOM)
        strcpy(option->value, _("Zoom"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t custom_zoom_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (rg_display_get_scaling() != RG_DISPLAY_SCALING_ZOOM)
    {
        option->flags = RG_DIALOG_FLAG_HIDDEN;
        return RG_DIALOG_VOID;
    }

    if (event == RG_DIALOG_PREV)
        rg_display_set_custom_zoom(rg_display_get_custom_zoom() - 0.05);
    if (event == RG_DIALOG_NEXT)
        rg_display_set_custom_zoom(rg_display_get_custom_zoom() + 0.05);

    sprintf(option->value, "%.2f", rg_display_get_custom_zoom());
    option->flags = RG_DIALOG_FLAG_NORMAL;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        return RG_DIALOG_REDRAW;
    return RG_DIALOG_VOID;
}

static rg_gui_event_t overclock_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    // if (event == RG_DIALOG_ENTER)
    // {
    //     const rg_gui_option_t options[] = {
    //         {0, _("CPU"), "-", RG_DIALOG_FLAG_NORMAL, &overclock_update_cb},
    //         {1, _("LCD"), "-", RG_DIALOG_FLAG_NORMAL, &overclock_update_cb},
    //         {2, _("SD"),  "-", RG_DIALOG_FLAG_NORMAL, &overclock_update_cb},
    //         RG_DIALOG_END,
    //     };
    //     rg_gui_dialog(option->label, options, 0);
    // }
    if (event == RG_DIALOG_PREV)
        rg_system_set_overclock(rg_system_get_overclock() - 1);
    else if (event == RG_DIALOG_NEXT)
        rg_system_set_overclock(rg_system_get_overclock() + 1);
    if (event == RG_DIALOG_INIT || event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
#if CONFIG_IDF_TARGET_ESP32S3
    {
        if (rg_system_get_app()->isLauncher)
            sprintf(option->value, "%d (games ~%dMhz; here 240)",
                    rg_system_get_overclock(), 240 + rg_system_get_overclock() * 10);
        else
            sprintf(option->value, "%d (%dMhz)", rg_system_get_overclock(), rg_system_get_cpu_speed());
    }
#else
        sprintf(option->value, "%d (%dMhz)", rg_system_get_overclock(), rg_system_get_cpu_speed());
#endif
    return RG_DIALOG_VOID;
}

static rg_gui_event_t speedup_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        float change = (event == RG_DIALOG_NEXT) ? 0.5f : -0.5f;
        rg_system_set_app_speed(rg_system_get_app_speed() + change);
    }
    sprintf(option->value, "%.1fx", rg_system_get_app_speed());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t led_indicator_opt_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        rg_system_set_indicator_mask(option->arg, !rg_system_get_indicator_mask(option->arg));
    }
    strcpy(option->value, rg_system_get_indicator_mask(option->arg) ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t led_indicator_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {RG_INDICATOR_ACTIVITY_SYSTEM, _("System activity"), "-", RG_DIALOG_FLAG_NORMAL, &led_indicator_opt_cb},
            {RG_INDICATOR_ACTIVITY_DISK,   _("Disk activity"),   "-", RG_DIALOG_FLAG_NORMAL, &led_indicator_opt_cb},
            {RG_INDICATOR_POWER_LOW,       _("Low battery"),     "-", RG_DIALOG_FLAG_NORMAL, &led_indicator_opt_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
    }
    return RG_DIALOG_VOID;
}

#ifdef RG_GPIO_VIBRATOR
static rg_gui_event_t haptic_enable_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool enabled = rg_system_get_haptic_enabled();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        enabled = !enabled;
        rg_system_set_haptic_enabled(enabled);
        if (enabled)
            rg_system_vibrate(RG_HAPTIC_INPUT_FEEDBACK_MS);
    }
    strcpy(option->value, enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t haptic_strength_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int strength = rg_system_get_haptic_strength();

    if (event == RG_DIALOG_PREV)
        strength -= 10;
    else if (event == RG_DIALOG_NEXT)
        strength += 10;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        rg_system_set_haptic_strength(strength);
        rg_system_vibrate(RG_HAPTIC_INPUT_FEEDBACK_MS);
    }

    strength = rg_system_get_haptic_strength();
    int bars = strength / 10;
    char slider[11];
    for (int i = 0; i < 10; ++i)
        slider[i] = i < bars ? '#' : '-';
    slider[10] = 0;

    snprintf(option->value, 24, "[%s] %d%%", slider, strength);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t haptic_test_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER || event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_system_vibrate(RG_HAPTIC_INPUT_FEEDBACK_MS);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t haptic_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {0, _("Enable"),   "-",  RG_DIALOG_FLAG_NORMAL, &haptic_enable_cb  },
            {0, _("Strength"), "-",  RG_DIALOG_FLAG_NORMAL, &haptic_strength_cb},
            {0, _("Test"),     NULL, RG_DIALOG_FLAG_NORMAL, &haptic_test_cb    },
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
    }
    return RG_DIALOG_VOID;
}
#endif

static rg_gui_event_t show_clock_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        gui.show_clock = !gui.show_clock;
        rg_settings_set_boolean(NS_GLOBAL, SETTING_CLOCK, gui.show_clock);
        return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, gui.show_clock ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t timezone_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char utc_offsets[][10] = {"UTC-12:00", "UTC-11:00", "UTC-10:00", "UTC-09:00", "UTC-09:30", "UTC-08:00",
                                    "UTC-07:00", "UTC-06:00", "UTC-05:00", "UTC-04:00", "UTC-03:30", "UTC-03:00",
                                    "UTC-02:00", "UTC-01:00", "UTC+00:00", "UTC+01:00", "UTC+02:00", "UTC+03:00",
                                    "UTC+03:30", "UTC+04:00", "UTC+04:30", "UTC+05:00", "UTC+05:30", "UTC+06:00",
                                    "UTC+06:30", "UTC+07:00", "UTC+08:00", "UTC+09:00", "UTC+09:30", "UTC+10:00",
                                    "UTC+10:30", "UTC+11:00", "UTC+12:00", "UTC+13:00", "UTC+14:00"};
    int index = 14, old_index = index;
    char *TZ = rg_system_get_timezone();
    if (TZ && strncmp(TZ, "UTC", 3) == 0)
    {
        // TZ has inverted offset for whatever reason
        TZ[3] = TZ[3] == '-' ? '+' : '-';
        for (size_t i = 0; i < RG_COUNT(utc_offsets); ++i)
        {
            if (strcmp(TZ, utc_offsets[i]) == 0)
            {
                index = old_index = i;
                break;
            }
        }
    }
    free(TZ);
    if (event == RG_DIALOG_NEXT && index < RG_COUNT(utc_offsets) - 1)
        index++;
    else if (event == RG_DIALOG_PREV && index > 0)
        index--;
    if (index != old_index)
    {
        char *TZ = strdup(utc_offsets[index]);
        TZ[3] = TZ[3] == '-' ? '+' : '-';
        rg_system_set_timezone(TZ);
        free(TZ);
        if (gui.show_clock)
            return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, utc_offsets[index]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t font_type_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV && rg_gui_set_font(gui.font_index - 1))
        return RG_DIALOG_REDRAW;
    if (event == RG_DIALOG_NEXT && rg_gui_set_font(gui.font_index + 1))
        return RG_DIALOG_REDRAW;
    if (gui.font_height != gui.font->height)
        sprintf(option->value, "%s (%d)", gui.font->name, gui.font_height);
    else
        sprintf(option->value, "%s", gui.font->name);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t theme_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        char *path = rg_gui_file_picker("Theme", RG_BASE_PATH_THEMES, NULL, false, true);
        if (path != NULL)
        {
            const char *theme = strlen(path) > 0 ? rg_basename(path) : NULL;
            rg_gui_set_theme(theme);
            free(path);
            return RG_DIALOG_REDRAW;
        }
    }

    strcpy(option->value, rg_gui_get_theme_name() ?: "Default");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t language_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int language_id = rg_localization_get_language_id();

    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t options[RG_LANG_MAX + 1];
        for (int i = 0; i < RG_LANG_MAX; i++)
            options[i] = (rg_gui_option_t){i, rg_localization_get_language_name(i), NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        options[RG_LANG_MAX] = (rg_gui_option_t)RG_DIALOG_END;

        int sel = rg_gui_dialog(option->label, options, language_id);
        if (sel != RG_DIALOG_CANCELLED)
        {
            rg_gui_set_language_id(sel);
            if (rg_gui_confirm(_("Language changed!"),
                               _("For these changes to take effect you must restart your device.\nrestart now?"), true))
            {
                rg_system_exit();
            }
            language_id = sel;
        }
        return RG_DIALOG_REDRAW;
    }

    sprintf(option->value, "%s", rg_localization_get_language_name(language_id) ?: "???");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t border_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        char *path = rg_gui_file_picker("Border", RG_BASE_PATH_BORDERS, NULL, false, true);
        if (path != NULL)
        {
            rg_display_set_border(strlen(path) ? path : NULL);
            free(path);
            return RG_DIALOG_REDRAW;
        }
    }
    char *border = rg_display_get_border();
    sprintf(option->value, "%.9s", border ? rg_basename(border) : _("None"));
    free(border);
    return RG_DIALOG_VOID;
}

#ifdef RG_ENABLE_NETWORKING
static void wifi_toggle_interactive(bool enable, int slot)
{
    rg_network_state_t target_state = enable ? RG_NETWORK_CONNECTED : RG_NETWORK_DISCONNECTED;
    int64_t timeout = rg_system_timer() + 20 * 1000000;
    rg_gui_draw_message(enable ? _("Connecting...") : _("Disconnecting..."));
    rg_network_wifi_stop();
    if (enable)
    {
        rg_wifi_config_t config = {0};
        rg_network_wifi_read_config(slot, &config);
        rg_network_wifi_set_config(&config);
        if (slot == 9000)
        {
            const rg_wifi_config_t config = {
                .ssid = "retro-go",
                .password = "retro-go",
                .channel = 6,
                .ap_mode = true,
            };
            rg_network_wifi_set_config(&config);
        }
        if (!rg_network_wifi_start())
            return;
    }
    do // Always loop at least once, in case we're in a transition
    {
        rg_task_delay(100);
        if (rg_system_timer() > timeout)
            break;
        if (rg_input_read_gamepad())
            break;
    } while (rg_network_get_info().state != target_state);
}

static rg_gui_event_t wifi_status_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_network_t info = rg_network_get_info();
    if (info.state != RG_NETWORK_CONNECTED)
        strcpy(option->value, _("Not connected"));
    else if (option->arg == 0x10)
        strcpy(option->value, info.name);
    else if (option->arg == 0x11)
        strcpy(option->value, info.ip_addr);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_manage_slot_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int slot = option->arg;
    rg_wifi_config_t config = {0};

    if (event == RG_DIALOG_INIT || event == RG_DIALOG_UPDATE || event == RG_DIALOG_ENTER)
    {
        rg_network_wifi_read_config(slot, &config);
        strcpy(option->value, config.ssid[0] ? config.ssid : _("(add network)"));
    }

    if (event == RG_DIALOG_ENTER)
    {
        if (!config.ssid[0])
        {
            // Get SSID from user
            char *ssid = rg_gui_input_str(_("Wi-Fi SSID"), _("Enter new network name:"), "");
            if (!ssid || strlen(ssid) == 0)
            {
                free(ssid);
                return RG_DIALOG_VOID;
            }

            // Get password from user
            char *password =
                rg_gui_input_str(_("Wi-Fi Password"), _("Enter password (leave empty for open network):"), "");
            if (!password)
                password = strdup("");

            // Save the configuration
            rg_wifi_config_t new_config = {0};
            strncpy(new_config.ssid, ssid, sizeof(new_config.ssid) - 1);
            strncpy(new_config.password, password, sizeof(new_config.password) - 1);
            new_config.channel = 0; // Auto
            new_config.ap_mode = false;

            free(ssid);
            free(password);

            if (!rg_network_wifi_write_config(slot, &new_config))
            {
                rg_gui_alert(_("Error"), _("Failed to save network configuration"));
                return RG_DIALOG_VOID;
            }

            rg_settings_commit();
            config = new_config;
            // fall through, allowing the user to connect to the new network
        }

        char title[50];
        snprintf(title, sizeof(title), "Slot %d: %.15s", slot, config.ssid);

        const rg_gui_option_t slot_options[] = {
            {1, _("Connect"),       NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {2, _("Edit SSID"),     NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {3, _("Edit Password"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {4, _("Delete"),        NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            RG_DIALOG_END,
        };

        int action = rg_gui_dialog(title, slot_options, 0);

        switch (action)
        {
        case 1: // Connect
            rg_settings_set_boolean(NS_WIFI, SETTING_WIFI_ENABLE, true);
            rg_settings_set_number(NS_WIFI, SETTING_WIFI_SLOT, slot);
            wifi_toggle_interactive(true, slot);
            break;

        case 2: // Edit SSID
        {
            char *new_ssid = rg_gui_input_str(_("Edit SSID"), _("Enter new network name:"), config.ssid);
            if (new_ssid && strlen(new_ssid) > 0)
            {
                strncpy(config.ssid, new_ssid, sizeof(config.ssid) - 1);
                config.ssid[sizeof(config.ssid) - 1] = '\0';
                rg_network_wifi_write_config(slot, &config);
                rg_settings_commit();
                rg_gui_alert(_("Success"), _("SSID updated"));
            }
            free(new_ssid);
            break;
        }

        case 3: // Edit Password
        {
            char *new_password = rg_gui_input_str(_("Edit Password"), _("Enter new password:"), config.password);
            if (new_password)
            {
                strncpy(config.password, new_password, sizeof(config.password) - 1);
                config.password[sizeof(config.password) - 1] = '\0';
                rg_network_wifi_write_config(slot, &config);
                rg_settings_commit();
                rg_gui_alert(_("Success"), _("Password updated"));
            }
            free(new_password);
            break;
        }

        case 4: // Delete
            if (rg_gui_confirm(_("Delete Network"), _("Are you sure you want to delete this network configuration?"),
                               false))
            {
                rg_network_wifi_delete_config(slot);
                rg_settings_commit();
                rg_gui_alert(_("Success"), _("Network configuration deleted"));
            }
            break;
        }

        strcpy(option->value, config.ssid[0] ? config.ssid : _("(empty)"));
        return RG_DIALOG_REDRAW;
    }

    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_manage_networks_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t slot_options[] = {
            {0, _("Slot 0"), "_", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {1, _("Slot 1"), "_", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {2, _("Slot 2"), "_", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {3, _("Slot 3"), "_", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {4, _("Slot 4"), "_", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(_("Manage Networks"), slot_options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_profile_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int slot = rg_settings_get_number(NS_WIFI, SETTING_WIFI_SLOT, -1);
    rg_wifi_config_t config;
    if (rg_network_wifi_read_config(slot, &config))
        sprintf(option->value, "%d - %s", slot, config.ssid);
    else
        strcpy(option->value, _("None"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_access_point_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        if (rg_gui_confirm(
                _("Wi-Fi AP"),
                _("Start access point?\n\nSSID: retro-go\nPassword: retro-go\n\nBrowse: http://192.168.4.1/"), true))
        {
            wifi_toggle_interactive(true, 9000);
        }
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_enable_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool enabled = rg_settings_get_boolean(NS_WIFI, SETTING_WIFI_ENABLE, false);
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        enabled = !enabled;
        rg_settings_set_boolean(NS_WIFI, SETTING_WIFI_ENABLE, enabled);
        wifi_toggle_interactive(enabled, rg_settings_get_number(NS_WIFI, SETTING_WIFI_SLOT, -1));
        return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {0x00, _("Wi-Fi enable"),       "-",  RG_DIALOG_FLAG_NORMAL,  &wifi_enable_cb         },
            {0x00, _("Manage networks"),    NULL, RG_DIALOG_FLAG_NORMAL,  &wifi_manage_networks_cb},
            RG_DIALOG_SEPARATOR,
            {0x00, _("Wi-Fi access point"), NULL, RG_DIALOG_FLAG_NORMAL,  &wifi_access_point_cb   },
            RG_DIALOG_SEPARATOR,
            {0x00, _("Wi-Fi profile"),      "-",  RG_DIALOG_FLAG_MESSAGE, &wifi_profile_cb        },
            {0x10, _("Network"),            "-",  RG_DIALOG_FLAG_MESSAGE, &wifi_status_cb         },
            {0x11, _("IP address"),         "-",  RG_DIALOG_FLAG_MESSAGE, &wifi_status_cb         },
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
    }
    return RG_DIALOG_VOID;
}
#endif

#ifdef RG_ENABLE_USB_HID_HOST
static rg_gui_event_t usb_msc_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    (void)option;
    if (event == RG_DIALOG_ENTER)
    {
        if (strcmp(rg_system_get_app()->name, "launcher") != 0)
        {
            rg_gui_alert(_("USB SD card"), _("Return to the launcher before sharing the SD card."));
            return RG_DIALOG_REDRAW;
        }
        if (rg_gui_confirm(_("Share SD card with computer?"),
                           _("The device will restart in USB drive mode. Safely eject the drive before leaving that mode."),
                           false))
        {
            rg_settings_commit();
            rg_gui_draw_message(_("Restarting in USB drive mode..."));
            rg_usb_msc_request();
        }
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t usb_hid_enable_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool enabled = rg_usb_hid_get_enabled();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        enabled = !enabled;
        rg_usb_hid_set_enabled(enabled);
    }
    strcpy(option->value, enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t usb_hid_status_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    (void)event;
    uint32_t connected = rg_usb_hid_get_connected();
    if (!connected)
        strcpy(option->value, _("None"));
    else
    {
        option->value[0] = 0;
        if (connected & (1U << RG_USB_HID_GAMEPAD)) strcat(option->value, "Pad ");
        if (connected & (1U << RG_USB_HID_KEYBOARD)) strcat(option->value, "Keyboard ");
        if (connected & (1U << RG_USB_HID_MOUSE)) strcat(option->value, "Mouse");
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t usb_hid_mapping_item_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_usb_hid_device_t device = (option->arg >> 8) & 0xFF;
    int key_index = option->arg & 0xFF;
    if (event == RG_DIALOG_ENTER)
    {
        const char *device_name = device == RG_USB_HID_GAMEPAD ? _("gamepad control") :
                                  device == RG_USB_HID_KEYBOARD ? _("keyboard key") : _("mouse action");
        rg_gui_draw_message(_("Press a USB %s for %s\n\nTimeout: 10 seconds"),
                            device_name, rg_input_get_key_name(1U << key_index));
        uint32_t source = 0;
        if (rg_usb_hid_capture_source(device, &source, 10000))
        {
            rg_usb_hid_set_mapping(device, key_index, source);
            rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
        }
        else
            rg_gui_alert(_("USB mapping"), _("No USB input was detected."));
        rg_display_force_redraw();
        return RG_DIALOG_REDRAW;
    }
    rg_usb_hid_source_name(device, rg_usb_hid_get_mapping(device, key_index), option->value, 32);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t usb_hid_reset_mapping_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER && rg_gui_confirm(_("Reset USB mappings?"), NULL, false))
    {
        rg_usb_hid_reset_mappings((rg_usb_hid_device_t)option->arg);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t usb_hid_mapping_menu_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    rg_usb_hid_device_t device = (rg_usb_hid_device_t)option->arg;
    rg_gui_option_t mappings_menu[RG_KEY_COUNT + 2];
    for (int i = 0; i < RG_KEY_COUNT; ++i)
    {
        mappings_menu[i] = (rg_gui_option_t){
            .arg = ((intptr_t)device << 8) | i,
            .label = rg_input_get_key_name(1U << i),
            .value = "-",
            .flags = RG_DIALOG_FLAG_NORMAL,
            .update_cb = usb_hid_mapping_item_cb,
        };
    }
    mappings_menu[RG_KEY_COUNT] = (rg_gui_option_t){device, _("Reset mappings"), NULL,
                                                     RG_DIALOG_FLAG_NORMAL, usb_hid_reset_mapping_cb};
    mappings_menu[RG_KEY_COUNT + 1] = (rg_gui_option_t)RG_DIALOG_END;
    rg_gui_dialog(option->label, mappings_menu, 0);
    return RG_DIALOG_REDRAW;
}

static rg_gui_event_t usb_hid_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t usb_options[] = {
            {0, _("USB HID input"), "-", RG_DIALOG_FLAG_NORMAL, usb_hid_enable_cb},
            {0, _("Connected"), "-", RG_DIALOG_FLAG_MESSAGE, usb_hid_status_cb},
            {RG_USB_HID_GAMEPAD, _("Gamepad mapping"), NULL, RG_DIALOG_FLAG_NORMAL, usb_hid_mapping_menu_cb},
            {RG_USB_HID_KEYBOARD, _("Keyboard mapping"), NULL, RG_DIALOG_FLAG_NORMAL, usb_hid_mapping_menu_cb},
            {RG_USB_HID_MOUSE, _("Mouse mapping"), NULL, RG_DIALOG_FLAG_NORMAL, usb_hid_mapping_menu_cb},
#ifdef RG_ENABLE_USB_MSC
            {0, _("USB SD card"), NULL, RG_DIALOG_FLAG_NORMAL, usb_msc_cb},
#endif
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, usb_options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}
#endif

#ifdef RG_ENABLE_USB_XINPUT
static rg_gui_event_t xinput_enable_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool enabled = rg_usb_xinput_get_enabled();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        enabled = !enabled;
        rg_usb_xinput_set_enabled(enabled);
    }
    strcpy(option->value, enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t xinput_status_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    (void)event;
    int count = (rg_usb_xinput_get_connected(0) ? 1 : 0) + (rg_usb_xinput_get_connected(1) ? 1 : 0);
    if (count == 0)
        strcpy(option->value, _("None"));
    else
        sprintf(option->value, "%d", count);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t xinput_mapping_item_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int key_index = option->arg & 0xFF;
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_draw_message(_("Press a control on the Xbox controller for %s\n\nTimeout: 10 seconds"),
                            rg_input_get_key_name(1U << key_index));
        uint32_t source = 0;
        if (rg_usb_xinput_capture_source(&source, 10000))
        {
            rg_usb_xinput_set_mapping(key_index, source);
            rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
        }
        else
            rg_gui_alert(_("Xbox controller mapping"), _("No input was detected."));
        rg_display_force_redraw();
        return RG_DIALOG_REDRAW;
    }
    rg_usb_xinput_source_name(rg_usb_xinput_get_mapping(key_index), option->value, 32);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t xinput_reset_mapping_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    (void)option;
    if (event == RG_DIALOG_ENTER && rg_gui_confirm(_("Reset Xbox controller mapping?"), NULL, false))
    {
        rg_usb_xinput_reset_mappings();
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t xinput_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t xinput_options[RG_KEY_COUNT + 4];
        xinput_options[0] = (rg_gui_option_t){0, _("Xbox controller input"), "-", RG_DIALOG_FLAG_NORMAL, xinput_enable_cb};
        xinput_options[1] = (rg_gui_option_t){0, _("Connected"), "-", RG_DIALOG_FLAG_MESSAGE, xinput_status_cb};
        for (int i = 0; i < RG_KEY_COUNT; ++i)
        {
            xinput_options[i + 2] = (rg_gui_option_t){
                .arg = i,
                .label = rg_input_get_key_name(1U << i),
                .value = "-",
                .flags = RG_DIALOG_FLAG_NORMAL,
                .update_cb = xinput_mapping_item_cb,
            };
        }
        xinput_options[RG_KEY_COUNT + 2] = (rg_gui_option_t){0, _("Reset mapping"), NULL,
                                                              RG_DIALOG_FLAG_NORMAL, xinput_reset_mapping_cb};
        xinput_options[RG_KEY_COUNT + 3] = (rg_gui_option_t)RG_DIALOG_END;
        rg_gui_dialog(option->label, xinput_options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}
#endif

#if defined(RG_ENABLE_USB_HID_HOST) || defined(RG_ENABLE_USB_XINPUT)
static void multiplayer_value_text(rg_input_source_t source, char *out, size_t out_size)
{
    int assignment = rg_input_source_get_assignment(source);
    const char *suffix = rg_input_source_connected(source) ? "" : _(" (unplugged)");
    if (assignment == RG_INPUT_PLAYER_AUTO)
    {
        int resolved = rg_input_source_get_player(source);
        snprintf(out, out_size, "%s%s", resolved == RG_PLAYER_1 ? _("Auto (P1)") :
                                        resolved == RG_PLAYER_2 ? _("Auto (P2)") : _("Auto (Off)"), suffix);
    }
    else
    {
        snprintf(out, out_size, "%s%s", assignment == RG_PLAYER_1 ? _("P1") :
                                        assignment == RG_PLAYER_2 ? _("P2") : _("Off"), suffix);
    }
}

static rg_gui_event_t multiplayer_item_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_input_source_t source = (rg_input_source_t)option->arg;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        static const int cycle[] = {RG_INPUT_PLAYER_AUTO, RG_INPUT_PLAYER_OFF, RG_PLAYER_1, RG_PLAYER_2};
        int assignment = rg_input_source_get_assignment(source);
        int index = 0;
        for (int i = 0; i < (int)RG_COUNT(cycle); ++i)
            if (cycle[i] == assignment)
                index = i;
        index = (index + (event == RG_DIALOG_PREV ? RG_COUNT(cycle) - 1 : 1)) % RG_COUNT(cycle);
        rg_input_source_set_assignment(source, cycle[index]);
    }
    multiplayer_value_text(source, option->value, 32);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t multiplayer_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t items[RG_INPUT_SOURCE_COUNT + 1];
        int count = 0;
        for (int s = 0; s < RG_INPUT_SOURCE_COUNT; ++s)
        {
            bool connected = rg_input_source_connected((rg_input_source_t)s);
            if (!connected && rg_input_source_get_assignment((rg_input_source_t)s) == RG_INPUT_PLAYER_AUTO)
                continue; // Never plugged in and not explicitly assigned, don't clutter the list
            items[count++] = (rg_gui_option_t){
                .arg = s,
                .label = rg_input_source_name((rg_input_source_t)s),
                .value = "-",
                .flags = RG_DIALOG_FLAG_NORMAL,
                .update_cb = multiplayer_item_cb,
            };
        }
        items[count++] = (rg_gui_option_t)RG_DIALOG_END;
        rg_gui_dialog(option->label, items, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}
#endif

static rg_gui_event_t app_options_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_app_t *app = rg_system_get_app();
        rg_gui_option_t options[24] = {
            {0, _("None"), NULL, RG_DIALOG_FLAG_MESSAGE, 0},
            RG_DIALOG_END,
        };
        if (app->handlers.options)
            app->handlers.options(options);
        rg_display_force_redraw();
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

void rg_gui_options_menu(void)
{
    rg_gui_option_t options[20] = {
#if RG_SCREEN_BACKLIGHT
        {0, _("Brightness"),      "-",  RG_DIALOG_FLAG_NORMAL, &brightness_update_cb},
#endif
        {0, _("Volume"),          "-",  RG_DIALOG_FLAG_NORMAL, &volume_update_cb    },
        {0, _("Audio out"),       "-",  RG_DIALOG_FLAG_NORMAL, &audio_update_cb     },
#ifdef RG_GPIO_VIBRATOR
        {0, _("Haptic feedback"), NULL, RG_DIALOG_FLAG_NORMAL, &haptic_cb           },
#endif
#ifdef RG_ENABLE_USB_HID_HOST
        {0, _("USB controllers"), NULL, RG_DIALOG_FLAG_NORMAL, &usb_hid_cb          },
#endif
#ifdef RG_ENABLE_USB_XINPUT
        {0, _("Xbox controller"), NULL, RG_DIALOG_FLAG_NORMAL, &xinput_cb          },
#endif
#if defined(RG_ENABLE_USB_HID_HOST) || defined(RG_ENABLE_USB_XINPUT)
        {0, _("Multiplayer"),     NULL, RG_DIALOG_FLAG_NORMAL, &multiplayer_cb     },
#endif
        RG_DIALOG_END,
    };
    const rg_gui_option_t misc_options[] = {
        {0, _("Font type"),        "-",  RG_DIALOG_FLAG_NORMAL, &font_type_cb    },
        {0, _("Theme"),            "-",  RG_DIALOG_FLAG_NORMAL, &theme_cb        },
        {0, _("Show clock"),       "-",  RG_DIALOG_FLAG_NORMAL, &show_clock_cb   },
        {0, _("Timezone"),         "-",  RG_DIALOG_FLAG_NORMAL, &timezone_cb     },
        {0, _("Language"),         "-",  RG_DIALOG_FLAG_NORMAL, &language_cb     },
#ifdef RG_GPIO_LED  // Only show disk LED option if disk LED GPIO pin is defined
        {0, _("LED options"),      NULL, RG_DIALOG_FLAG_NORMAL, &led_indicator_cb},
#endif
#ifdef RG_ENABLE_NETWORKING
        {0, _("Wi-Fi options"),    NULL, RG_DIALOG_FLAG_NORMAL, &wifi_cb         },
#endif
        {0, _("Launcher options"), NULL, RG_DIALOG_FLAG_NORMAL, &app_options_cb  },
        RG_DIALOG_END,
    };
    const rg_gui_option_t game_options[] = {
        {0, _("Scaling"),          "-",  RG_DIALOG_FLAG_NORMAL, &scaling_update_cb},
        {0, _("Factor"),           "-",  RG_DIALOG_FLAG_HIDDEN, &custom_zoom_cb   },
        {0, _("Filter"),           "-",  RG_DIALOG_FLAG_NORMAL, &filter_update_cb },
        {0, _("Border"),           "-",  RG_DIALOG_FLAG_NORMAL, &border_update_cb },
        {0, _("Speed"),            "-",  RG_DIALOG_FLAG_NORMAL, &speedup_update_cb},
// {0, _("Misc options"),  NULL, RG_DIALOG_FLAG_NORMAL, &misc_options_cb},
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S3  // && !RG_BUILD_RELEASE
        {0, _("Overclock"),        "-",  RG_DIALOG_FLAG_NORMAL, &overclock_cb     },
#endif
        {0, _("Emulator options"), NULL, RG_DIALOG_FLAG_NORMAL, &app_options_cb   },
        RG_DIALOG_END,
    };

    const rg_app_t *app = rg_system_get_app();
    if (app->isLauncher)
        memcpy(options + get_dialog_items_count(options), misc_options, sizeof(misc_options));
    else
        memcpy(options + get_dialog_items_count(options), game_options, sizeof(game_options));

    rg_audio_set_mute(true);

    rg_gui_dialog(_("Options"), options, 0);
    rg_settings_commit();

    rg_audio_set_mute(false);
}

void rg_gui_about_menu(void)
{
    const rg_app_t *app = rg_system_get_app();
    // bool have_option_btn = rg_input_key_is_present(RG_KEY_OPTION);
    //  TODO: Add indicator whether or not the build is a release, and if it's official (built by me)
    rg_gui_option_t options[20] = {
        {0, _("Version"),        (char *)app->version,       RG_DIALOG_FLAG_NORMAL, NULL},
        {0, _("Date"),           (char *)app->buildDate,     RG_DIALOG_FLAG_NORMAL, NULL},
        {0, _("Target"),         (char *)RG_TARGET_NAME,     RG_DIALOG_FLAG_NORMAL, NULL},
        {0, _("Website"),        (char *)RG_PROJECT_WEBSITE, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_SEPARATOR,
        {4, _("Options"),        NULL,                       RG_DIALOG_FLAG_NORMAL, NULL},
        //{4, _("Options"),        NULL,                       have_option_btn ? RG_DIALOG_FLAG_HIDDEN :
        //RG_DIALOG_FLAG_NORMAL, NULL},
        // {1, _("View credits", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, _("Debug menu"),     NULL,                       RG_DIALOG_FLAG_NORMAL, NULL},
        {3, _("Reset settings"), NULL,                       RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };

    if (app->handlers.about)
        app->handlers.about(options + get_dialog_items_count(options));

    while (true)
    {
        switch (rg_gui_dialog(_("About Retro-Go"), options, 4))
        {
        case 1:
            // FIXME: This should probably be a regular dialog so that it's scrollable!
            rg_gui_alert("Credits", RG_PROJECT_CREDITS);
            break;
        case 2:
            rg_gui_debug_menu();
            break;
        case 3:
            if (rg_gui_confirm(_("Reset all settings?"), NULL, false))
            {
                rg_system_clear_cache();
                rg_settings_reset();
                rg_system_restart();
                return;
            }
            break;
        case 4:
            rg_gui_options_menu();
            break;
        default:
            return;
        }
    }
}

void rg_gui_debug_menu(void)
{
    char screen_res[20], source_res[20], scaled_res[20];
    char stack_hwm[20], heap_free[20], block_free[20];
    char local_time[32], timezone[32], uptime[20];
    char battery_info[20], frame_time[20], overclock[20];
    char app_name[32], network_str[64];

    const rg_gui_option_t options[] = {
        {0x100, "Screen res",         screen_res,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Source res",         source_res,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Scaled res",         scaled_res,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Stack HWM ",         stack_hwm,    RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Heap free ",         heap_free,    RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Block free",         block_free,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "App name  ",         app_name,     RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Network   ",         network_str,  RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Local time",         local_time,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Timezone  ",         timezone,     RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Uptime    ",         uptime,       RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Battery   ",         battery_info, RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Blit time ",         frame_time,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0x000, "Overclock",          overclock,    RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_SEPARATOR,
        {0x001, "Reboot to recovery", NULL,         RG_DIALOG_FLAG_NORMAL, NULL},
        {0x002, "Clear cache    ",    NULL,         RG_DIALOG_FLAG_NORMAL, NULL},
        {0x003, "Save screenshot",    NULL,         RG_DIALOG_FLAG_NORMAL, NULL},
        {0x004, "Save trace",         NULL,         RG_DIALOG_FLAG_NORMAL, NULL},
        {0x005, "Cheats    ",         NULL,         RG_DIALOG_FLAG_NORMAL, NULL},
        {0x006, "Crash     ",         NULL,         RG_DIALOG_FLAG_NORMAL, NULL},
        {0x007, "Log=debug ",         NULL,         RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };

    const rg_display_t *display = rg_display_get_info();
    rg_display_counters_t display_stats = rg_display_get_counters();
    rg_stats_t stats = rg_system_get_stats();
    time_t now = time(NULL);

    strftime(local_time, 32, "%F %T", localtime(&now));
    snprintf(timezone, 32, "%s", getenv("TZ") ?: "N/A");
    snprintf(screen_res, 20, "%dx%d", display->screen.width, display->screen.height);
    snprintf(source_res, 20, "%dx%d", display->source.width, display->source.height);
    snprintf(scaled_res, 20, "%dx%d", display->viewport.width, display->viewport.height);
    if (display_stats.totalFrames > 0)
    {
        int total = (float)display_stats.busyTime / display_stats.totalFrames / 1000.f;
        int block = (float)display_stats.blockTime / display_stats.totalFrames / 1000.f;
        snprintf(frame_time, 20, "%dms (block: %dms)", total, block);
    }
    else
        snprintf(frame_time, 20, "N/A");
    snprintf(stack_hwm, 20, "%d", stats.freeStackMain);
    snprintf(heap_free, 20, "%d+%d", stats.freeMemoryInt, stats.freeMemoryExt);
    snprintf(block_free, 20, "%d+%d", stats.freeBlockInt, stats.freeBlockExt);
    snprintf(app_name, 32, "%s", rg_system_get_app()->name);
    snprintf(uptime, 20, "%ds", stats.uptime);
    snprintf(overclock, 20, "%d (%dMhz)", rg_system_get_overclock(), rg_system_get_cpu_speed());

    rg_battery_t battery;
    if (rg_input_read_battery_raw(&battery))
        snprintf(battery_info, sizeof(battery_info), "%.2f%% | %.2fV", battery.level, battery.volts);
    else
        snprintf(battery_info, sizeof(battery_info), "N/A");

    rg_network_t net = rg_network_get_info();
    if (net.state == RG_NETWORK_DISABLED)
        snprintf(network_str, 64, "%s", "not available");
    else if (net.state == RG_NETWORK_CONNECTED)
        snprintf(network_str, 64, "%s\n%s", net.name, net.ip_addr);
    else if (net.state == RG_NETWORK_CONNECTING)
        snprintf(network_str, 64, "%s\n%s", net.name, "connecting...");
    else if (net.name[0])
        snprintf(network_str, 64, "%s\n%s", net.name, "disconnected");
    else
        snprintf(network_str, 64, "%s", "disconnected");

    switch (rg_gui_dialog("Debugging", options, 0))
    {
    case 0x001:
        rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, RG_BOOT_RECOVERY);
        break;
    case 0x002:
        // Everything cached anywhere on the card, not just this folder: rom lists, checksums, the
        // media library index, per-emulator cache files. It restarts because half the app is holding
        // data that just went away.
        if (rg_gui_confirm(_("Clear cache"), _("Delete all cached data and restart?"), true))
        {
            rg_system_clear_cache();
            rg_system_restart();
        }
        break;
    case 0x003:
        rg_emu_screenshot(RG_STORAGE_ROOT "/screenshot.png", 0, 0);
        break;
    case 0x004:
        rg_system_save_trace(RG_STORAGE_ROOT "/trace.txt", 0);
        break;
    case 0x005:
        break;
    case 0x006:
        RG_PANIC("Crash test!");
        break;
    case 0x007:
        rg_system_set_log_level(RG_LOG_DEBUG);
        break;
    }
}

static rg_gui_event_t slot_select_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_emu_slot_t *slot = (rg_emu_slot_t *)option->arg;
    if (event == RG_DIALOG_FOCUS_GAINED)
    {
        rg_image_t *preview = NULL;
        rg_color_t color = gui.palette.accent;
        size_t margin = 0; // TEXT_RECT("ABC", 0).height;
        size_t border = 3;
        char buffer[100];
        if (slot->is_used)
        {
            preview = rg_surface_load_image_file(slot->preview, 0);
            if (slot->is_lastused)
                snprintf(buffer, sizeof(buffer), "Slot %d (last used)", slot->id);
            else
                snprintf(buffer, sizeof(buffer), "Slot %d", slot->id);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "Slot %d is empty", slot->id);
            color = C_RGB(240, 90, 90);
        }
        // The frame stays a semantic used/empty indicator (accent when there is a state to load,
        // red when there is not); the label chip follows the theme like every other dialog.
        int text_height = gui.font_height * 2 + 4;
        int chip_height = text_height + 8;
        int chip_width = gui.screen_width - border * 2 - 16;
        rg_gui_draw_image(0, margin, gui.screen_width, gui.screen_height - margin * 2, true, preview);
        rg_gui_draw_rect(0, margin, gui.screen_width, gui.screen_height - margin * 2, border, color, C_NONE);
        rg_gui_draw_shadow(border + 8, margin + border + 6, chip_width, chip_height, 5, 2);
        rg_gui_draw_panel(border + 8, margin + border + 6, chip_width, chip_height, 5, gui.style.box_background, color,
                          255);
        rg_gui_draw_text(border + 18, margin + border + 6 + (chip_height - text_height) / 2, chip_width - 20, buffer,
                         gui.style.item_standard, gui.style.box_background,
                         RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER | RG_TEXT_NO_PADDING);
        rg_surface_free(preview);
    }
    else if (event == RG_DIALOG_ENTER)
    {
        return RG_DIALOG_SELECT;
    }
    return RG_DIALOG_VOID;
#undef draw_status
}

int rg_gui_savestate_menu(const char *title, const char *rom_path)
{
    rg_emu_states_t *savestates = rg_emu_get_states(rom_path, 4);
    const rg_gui_option_t choices[] = {
        {(intptr_t)&savestates->slots[0], _("Slot 0"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        {(intptr_t)&savestates->slots[1], _("Slot 1"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        {(intptr_t)&savestates->slots[2], _("Slot 2"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        {(intptr_t)&savestates->slots[3], _("Slot 3"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        RG_DIALOG_END
    };

    intptr_t ret = rg_gui_dialog(title, choices, savestates->lastused ? savestates->lastused->id : 0);
    int slot = (ret == RG_DIALOG_CANCELLED) ? -1 : ((rg_emu_slot_t *)ret)->id;
    free(savestates);
    return slot;
}

void rg_gui_game_menu(void)
{
    const char *rom_path = rg_system_get_app()->romPath;
    const rg_gui_option_t choices[] = {
        {1000, _("Save & Continue"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2000, _("Save & Quit"),     NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3001, _("Load game"),       NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3000, _("Reset"),           NULL, RG_DIALOG_FLAG_NORMAL, NULL},
#ifdef RG_ENABLE_NETPLAY
        {5000, _("Netplay"),         NULL, RG_DIALOG_FLAG_NORMAL, NULL},
#endif
        {5500, _("Options"),         NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {6000, _("About"),           NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {7000, _("Quit"),            NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };
    int slot, sel;

    rg_audio_set_mute(true);

    sel = rg_gui_dialog("Retro-Go", choices, 0);

    if (sel == 3000)
    {
        const rg_gui_option_t choices[] = {
            {3002, _("Soft reset"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {3003, _("Hard reset"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            RG_DIALOG_END
        };
        sel = rg_gui_dialog(_("Reset Emulation?"), choices, 0);
    }

    switch (sel)
    {
    case 1000:
        if ((slot = rg_gui_savestate_menu(_("Save"), rom_path)) >= 0)
            rg_emu_save_state(slot);
        break;
    case 2000:
        if ((slot = rg_gui_savestate_menu(_("Save"), rom_path)) >= 0 && rg_emu_save_state(slot))
            rg_system_exit();
        break;
    case 3001:
        if ((slot = rg_gui_savestate_menu(_("Load"), rom_path)) >= 0)
            rg_emu_load_state(slot);
        break;
    case 3002:
        rg_emu_reset(false);
        break;
    case 3003:
        rg_emu_reset(true);
        break;
#ifdef RG_ENABLE_NETPLAY
    case 5000:
        rg_netplay_quick_start();
        break;
#endif
    case 5500:
        rg_gui_options_menu();
        break;
    case 6000:
        rg_gui_about_menu();
        break;
    case 7000:
        rg_system_exit();
        break;
    }

    rg_audio_set_mute(false);
}
