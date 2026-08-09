#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media.h"
#include "media_audio.h"
#include "media_fft.h"
#include "media_ui_internal.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_UI"

media_ui_t mui;

/* Set by media_run_at() before the loop starts, consumed once on entry. */
static int pending_view = -1;

void media_ui_set_pending_view(media_browse_mode_t mode)
{
    pending_view = (int)mode;
}

/* Long-press / repeat handling. Retro-go's launcher uses a 400 ms initial delay, so the
 * player matches it: the same button feels the same everywhere. */
#define REPEAT_DELAY_US 400000
#define OVERLAY_MS 1400

/* -------------------------------------------------------------------------------------- */
/* Theme and layout                                                                         */
/* -------------------------------------------------------------------------------------- */

void media_ui_update_theme(void)
{
    const media_settings_t *cfg = media_settings();
    media_palette_t palette = {0};

    if (cfg->dynamic_theme && mui.snapshot.state != MEDIA_STATE_STOPPED)
    {
        const char *path = media_player_path();
        if (path)
            palette = media_artwork_palette(path);
    }

    // A dark base is the constant; only the accents follow the artwork. That keeps contrast
    // predictable no matter what a cover happens to look like.
    mui.theme.background = C_BLACK;
    mui.theme.surface = C_RGB(22, 22, 26);
    mui.theme.text = C_RGB(240, 240, 244);
    mui.theme.text_dim = C_RGB(140, 140, 150);
    mui.theme.divider = C_RGB(48, 48, 54);

    if (palette.valid)
    {
        mui.theme.accent = palette.primary;
        mui.theme.accent_dim = media_color_scale(palette.primary, 110);
        mui.theme.highlight = palette.highlight;
        mui.theme.surface = media_color_blend(mui.theme.surface, palette.background, 120);
    }
    else
    {
        mui.theme.accent = C_RGB(90, 170, 255);
        mui.theme.accent_dim = C_RGB(50, 100, 160);
        mui.theme.highlight = C_RGB(160, 205, 255);
    }

    // Guarantee the accent is legible on the surface colour whatever the artwork did.
    if (abs(media_color_luma(mui.theme.accent) - media_color_luma(mui.theme.surface)) < 60)
        mui.theme.accent = media_color_blend(mui.theme.accent, C_WHITE, 120);
}

static void compute_layout(void)
{
    media_layout_t *l = &mui.layout;

    l->width = rg_display_get_width();
    l->height = rg_display_get_height();
    l->safe = rg_gui_get_safe_area();
    l->line_h = RG_MAX(rg_gui_get_font_height(), 8);
    l->pad = RG_MAX(l->width / 60, 3);

    l->header_h = l->line_h + l->pad * 2;
    l->footer_h = l->line_h + l->pad;
    l->mini_h = 0;

    l->content_top = l->header_h;
    l->content_h = l->height - l->header_h - l->footer_h;
    if (l->content_h < l->line_h * 3)
        l->content_h = l->line_h * 3;
}

/* -------------------------------------------------------------------------------------- */
/* Drawing primitives                                                                       */
/* -------------------------------------------------------------------------------------- */

void media_ui_clear(void)
{
    const media_settings_t *cfg = media_settings();
    const rg_image_t *background = NULL;

    if (cfg->artwork_background && !cfg->low_effects && mui.snapshot.state != MEDIA_STATE_STOPPED)
    {
        const char *path = media_player_path();
        if (path)
            background = media_artwork_background(path, mui.layout.width, mui.layout.height);
    }

    if (background)
        rg_gui_draw_image(0, 0, mui.layout.width, mui.layout.height, false, background);
    else
        rg_gui_draw_rect(0, 0, mui.layout.width, mui.layout.height, 0, 0, mui.theme.background);
}

void media_ui_draw_panel(int x, int y, int w, int h, rg_color_t fill, rg_color_t border)
{
    if (w <= 0 || h <= 0)
        return;

    // The renderer has no rounded-rectangle primitive, so the corners are faked by insetting
    // the top and bottom rows by a pixel. At this scale it reads as a rounded card.
    rg_gui_draw_rect(x + 1, y, w - 2, 1, 0, 0, fill);
    rg_gui_draw_rect(x, y + 1, w, h - 2, 0, 0, fill);
    rg_gui_draw_rect(x + 1, y + h - 1, w - 2, 1, 0, 0, fill);

    if (border != C_NONE && border != fill)
    {
        rg_gui_draw_rect(x + 1, y, w - 2, 1, 0, 0, border);
        rg_gui_draw_rect(x + 1, y + h - 1, w - 2, 1, 0, 0, border);
        rg_gui_draw_rect(x, y + 1, 1, h - 2, 0, 0, border);
        rg_gui_draw_rect(x + w - 1, y + 1, 1, h - 2, 0, 0, border);
    }
}

void media_ui_draw_progress(int x, int y, int w, int h, int percent, rg_color_t fill, rg_color_t bg)
{
    if (w <= 2 || h <= 0)
        return;

    percent = media_clampi(percent, 0, 100);
    int filled = (w * percent) / 100;

    rg_gui_draw_rect(x, y, w, h, 0, 0, bg);
    if (filled > 0)
        rg_gui_draw_rect(x, y, filled, h, 0, 0, fill);

    // Playhead knob, which is what makes the bar read as scrubbable
    int knob = RG_MAX(h + 2, 5);
    int knob_x = media_clampi(x + filled - knob / 2, x, x + w - knob);
    rg_gui_draw_rect(knob_x, y - (knob - h) / 2, knob, knob, 0, 0, fill);
}

void media_ui_draw_scrollbar(int x, int y, int h, int visible, int total, int offset)
{
    if (total <= visible || h <= 4)
        return;

    rg_gui_draw_rect(x, y, 2, h, 0, 0, mui.theme.divider);

    int thumb = RG_MAX((h * visible) / total, 6);
    int travel = h - thumb;
    int position = total > visible ? (travel * offset) / (total - visible) : 0;
    rg_gui_draw_rect(x, y + media_clampi(position, 0, travel), 2, thumb, 0, 0, mui.theme.accent);
}

void media_ui_draw_header(const char *title, const char *right)
{
    media_layout_t *l = &mui.layout;

    rg_gui_draw_rect(0, 0, l->width, l->header_h, 0, 0, media_color_scale(mui.theme.surface, 200));
    rg_gui_draw_rect(0, l->header_h - 1, l->width, 1, 0, 0, mui.theme.divider);

    rg_gui_draw_text(l->pad * 2, l->pad, l->width / 2, title ?: "", mui.theme.text, C_TRANSPARENT,
                     RG_TEXT_ALIGN_LEFT);

    if (right && *right)
        rg_gui_draw_text(l->width / 2, l->pad, l->width / 2 - l->pad * 2, right, mui.theme.text_dim,
                         C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
}

void media_ui_draw_footer(const char *hints)
{
    media_layout_t *l = &mui.layout;
    int y = l->height - l->footer_h;

    rg_gui_draw_rect(0, y, l->width, l->footer_h, 0, 0, media_color_scale(mui.theme.surface, 170));
    rg_gui_draw_rect(0, y, l->width, 1, 0, 0, mui.theme.divider);
    rg_gui_draw_text(l->pad * 2, y + l->pad / 2, l->width - l->pad * 4, hints ?: "",
                     mui.theme.text_dim, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
}

/**
 * Draw text that scrolls only when it does not fit and only after a pause. Scrolling every
 * label all the time is noise; scrolling the one the user is looking at is information.
 */
void media_ui_draw_marquee(int x, int y, int w, const char *text, rg_color_t color, uint32_t flags,
                           bool active)
{
    if (!text || !*text || w <= 0)
        return;

    rg_rect_t size = TEXT_RECT(text, 0);

    if (size.width <= w || !active)
    {
        rg_gui_draw_text(x, y, w, text, color, C_TRANSPARENT, flags | RG_TEXT_ALIGN_LEFT);
        return;
    }

    // Time-based, so the scroll speed is identical at 30 and 60 fps.
    int64_t elapsed_ms = (mui.frame_us - mui.marquee_reset_at) / 1000;
    const int64_t hold_ms = 1200;
    const int speed_px_s = 30;

    int overflow = size.width - w + 16;
    int64_t travel_ms = ((int64_t)overflow * 1000) / speed_px_s;
    int64_t cycle = hold_ms * 2 + travel_ms * 2;
    int64_t phase = travel_ms > 0 ? (elapsed_ms % cycle) : 0;

    int offset = 0;
    if (phase < hold_ms)
        offset = 0;
    else if (phase < hold_ms + travel_ms)
        offset = (int)(((phase - hold_ms) * overflow) / travel_ms);
    else if (phase < hold_ms * 2 + travel_ms)
        offset = overflow;
    else
        offset = overflow - (int)(((phase - hold_ms * 2 - travel_ms) * overflow) / travel_ms);

    // Clip by drawing into the band and letting the caller's panel cover the overspill.
    rg_gui_draw_text(x - offset, y, size.width + 8, text, color, C_TRANSPARENT,
                     flags | RG_TEXT_ALIGN_LEFT);
}

void media_ui_show_overlay(const char *title, const char *value, int percent)
{
    media_utf8_copy(mui.overlay_title, sizeof(mui.overlay_title), title ?: "");
    media_utf8_copy(mui.overlay_value, sizeof(mui.overlay_value), value ?: "");
    mui.overlay_percent = percent;
    mui.overlay_until_us = rg_system_timer() + OVERLAY_MS * 1000;
    mui.needs_redraw = true;
}

void media_ui_draw_overlay(void)
{
    if (mui.frame_us > mui.overlay_until_us)
        return;

    media_layout_t *l = &mui.layout;
    int w = media_clampi(l->width * 2 / 3, 120, l->width - l->pad * 4);
    int h = l->line_h * 2 + l->pad * 4;
    int x = (l->width - w) / 2;
    int y = l->height - l->footer_h - h - l->pad * 2;

    media_ui_draw_panel(x, y, w, h, media_color_scale(mui.theme.surface, 235), mui.theme.divider);

    rg_gui_draw_text(x + l->pad * 2, y + l->pad, w - l->pad * 4, mui.overlay_title, mui.theme.text,
                     C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(x + l->pad * 2, y + l->pad, w - l->pad * 4, mui.overlay_value,
                     mui.theme.accent, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);

    if (mui.overlay_percent >= 0)
        media_ui_draw_progress(x + l->pad * 2, y + l->pad * 2 + l->line_h, w - l->pad * 4, 4,
                               mui.overlay_percent, mui.theme.accent, mui.theme.divider);
}

void media_ui_draw_message(const char *title, const char *body)
{
    media_layout_t *l = &mui.layout;
    int w = media_clampi(l->width - l->pad * 8, 100, l->width);
    rg_rect_t measured = body ? TEXT_RECT(body, w - l->pad * 4) : (rg_rect_t){0};
    int h = l->line_h + measured.height + l->pad * 5;
    int x = (l->width - w) / 2;
    int y = (l->height - h) / 2;

    media_ui_draw_panel(x, y, w, h, mui.theme.surface, mui.theme.divider);
    rg_gui_draw_text(x + l->pad * 2, y + l->pad * 2, w - l->pad * 4, title ?: "", mui.theme.text,
                     C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    if (body)
        rg_gui_draw_text(x + l->pad * 2, y + l->pad * 3 + l->line_h, w - l->pad * 4, body,
                         mui.theme.text_dim, C_TRANSPARENT,
                         RG_TEXT_ALIGN_CENTER | RG_TEXT_MULTILINE);
}

/**
 * Album art with a graceful placeholder. The placeholder is derived from the track's own
 * hash, so a coverless album still gets a stable, distinctive colour instead of grey.
 */
void media_ui_draw_art(int x, int y, int size, const char *path, const media_palette_t *palette,
                       const char *fallback_text)
{
    const rg_image_t *art = path ? media_artwork_get(path, media_profile()->artwork_max_dim, true)
                                 : NULL;

    if (art)
    {
        // Preserve the aspect ratio inside the square slot rather than stretching the cover.
        int w = size, h = size;
        if (art->width > art->height)
            h = media_clampi(size * art->height / art->width, 1, size);
        else if (art->height > art->width)
            w = media_clampi(size * art->width / art->height, 1, size);

        rg_gui_draw_image(x + (size - w) / 2, y + (size - h) / 2, w, h, true, art);
        return;
    }

    rg_color_t fill = palette && palette->valid ? media_color_scale(palette->primary, 70)
                                                : mui.theme.surface;
    media_ui_draw_panel(x, y, size, size, fill, mui.theme.divider);

    // Two stylised note heads: recognisable at 40 px and free of any font dependency.
    int cx = x + size / 2;
    int cy = y + size / 2;
    int stem = RG_MAX(size / 12, 2);
    int head = RG_MAX(size / 7, 4);
    rg_color_t ink = palette && palette->valid ? palette->highlight : mui.theme.text_dim;

    rg_gui_draw_rect(cx - head, cy - size / 5, stem, size / 3, 0, 0, ink);
    rg_gui_draw_rect(cx + head, cy - size / 4, stem, size / 3 + size / 12, 0, 0, ink);
    rg_gui_draw_rect(cx - head, cy - size / 5, head * 2 + stem, stem, 0, 0, ink);
    rg_gui_draw_rect(cx - head - head, cy + size / 8, head + 2, head, 0, 0, ink);
    rg_gui_draw_rect(cx + head - head / 2, cy + size / 12, head + 2, head, 0, 0, ink);

    if (fallback_text && *fallback_text)
        rg_gui_draw_text(x, y + size - mui.layout.line_h - 2, size, fallback_text,
                         mui.theme.text_dim, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

/* -------------------------------------------------------------------------------------- */
/* Mini player                                                                              */
/* -------------------------------------------------------------------------------------- */

void media_ui_draw_mini_player(void)
{
    if (mui.snapshot.state == MEDIA_STATE_STOPPED || !mui.track)
        return;

    media_layout_t *l = &mui.layout;
    int h = l->line_h * 2 + l->pad * 2;
    int y = l->height - l->footer_h - h;
    int art = h - l->pad * 2;

    media_ui_draw_panel(0, y, l->width, h, media_color_scale(mui.theme.surface, 225),
                        mui.theme.divider);

    media_palette_t palette = media_artwork_palette(media_player_path());
    media_ui_draw_art(l->pad, y + l->pad, art, media_player_path(), &palette, NULL);

    int text_x = l->pad * 2 + art;
    int text_w = l->width - text_x - l->pad * 8;

    rg_gui_draw_text(text_x, y + l->pad / 2, text_w, mui.track->title, mui.theme.text,
                     C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(text_x, y + l->pad / 2 + l->line_h, text_w,
                     mui.track->artist[0] ? mui.track->artist : "Unknown Artist",
                     mui.theme.text_dim, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);

    // Transport glyph, drawn rather than typed so it never depends on the font
    int gx = l->width - l->pad * 5;
    int gy = y + h / 2 - l->line_h / 3;
    int gs = RG_MAX(l->line_h / 2, 4);

    if (mui.snapshot.state == MEDIA_STATE_PLAYING || mui.snapshot.state == MEDIA_STATE_BUFFERING)
    {
        rg_gui_draw_rect(gx, gy, gs / 2, gs * 2, 0, 0, mui.theme.accent);
        rg_gui_draw_rect(gx + gs, gy, gs / 2, gs * 2, 0, 0, mui.theme.accent);
    }
    else
    {
        for (int i = 0; i < gs; ++i)
            rg_gui_draw_rect(gx + i, gy + i / 2, 1, gs * 2 - i, 0, 0, mui.theme.accent);
    }

    int percent = mui.snapshot.duration_ms
                      ? (int)((uint64_t)mui.snapshot.position_ms * 100 / mui.snapshot.duration_ms)
                      : 0;
    rg_gui_draw_rect(0, y + h - 2, (l->width * media_clampi(percent, 0, 100)) / 100, 2, 0, 0,
                     mui.theme.accent);

    l->mini_h = h;
}

/* -------------------------------------------------------------------------------------- */
/* Generic list                                                                             */
/* -------------------------------------------------------------------------------------- */

void media_list_reset(media_list_t *list)
{
    list->count = 0;
    list->cursor = 0;
    list->scroll = 0;
}

media_list_item_t *media_list_add(media_list_t *list)
{
    if (list->count >= list->capacity)
    {
        int capacity = list->capacity ? list->capacity * 2 : 64;
        media_list_item_t *items = realloc(list->items, (size_t)capacity * sizeof(media_list_item_t));
        if (!items)
            return NULL;
        list->items = items;
        list->capacity = capacity;
    }

    media_list_item_t *item = &list->items[list->count++];
    memset(item, 0, sizeof(*item));
    return item;
}

void media_list_free(media_list_t *list)
{
    free(list->items);
    memset(list, 0, sizeof(*list));
}

int media_list_visible_rows(void)
{
    int available = mui.layout.content_h - mui.layout.mini_h;
    return RG_MAX(available / (mui.layout.line_h + 2), 1);
}

void media_list_move(media_list_t *list, int delta, int page)
{
    if (list->count <= 0)
    {
        list->cursor = 0;
        return;
    }

    int rows = media_list_visible_rows();
    int cursor = list->cursor + (page ? delta * rows : delta);

    if (page)
        cursor = media_clampi(cursor, 0, list->count - 1);
    else if (cursor < 0)
        cursor = list->count - 1; // Wrap, like the launcher's own lists
    else if (cursor >= list->count)
        cursor = 0;

    list->cursor = cursor;

    if (list->cursor < list->scroll)
        list->scroll = list->cursor;
    else if (list->cursor >= list->scroll + rows)
        list->scroll = list->cursor - rows + 1;

    list->scroll = media_clampi(list->scroll, 0, RG_MAX(list->count - rows, 0));

    mui.marquee_reset_at = mui.frame_us;
}

/* -------------------------------------------------------------------------------------- */
/* Quick settings overlay                                                                   */
/* -------------------------------------------------------------------------------------- */

enum
{
    QUICK_VOLUME = 0,
    QUICK_BRIGHTNESS,
    QUICK_EQ,
    QUICK_SHUFFLE,
    QUICK_REPEAT,
    QUICK_VISUALIZER,
    QUICK_COUNT,
};

static void quick_adjust(int row, int delta)
{
    media_settings_t *cfg = media_settings();

    switch (row)
    {
    case QUICK_VOLUME:
        rg_audio_set_volume(media_clampi(rg_audio_get_volume() + delta * 5, 0, 100));
        break;

    case QUICK_BRIGHTNESS:
        // Uses the system backlight so there is exactly one brightness in the firmware.
        rg_display_set_backlight(media_clampi(rg_display_get_backlight() + delta * 10,
                                              RG_DISPLAY_BACKLIGHT_MIN, RG_DISPLAY_BACKLIGHT_MAX));
        break;

    case QUICK_EQ:
        cfg->eq_enabled = !cfg->eq_enabled;
        media_eq_set_enabled(cfg->eq_enabled);
        break;

    case QUICK_SHUFFLE:
        cfg->shuffle = !cfg->shuffle;
        media_player_set_shuffle(cfg->shuffle);
        break;

    case QUICK_REPEAT:
    {
        int repeat = ((int)cfg->repeat + delta + MEDIA_REPEAT_COUNT) % MEDIA_REPEAT_COUNT;
        media_player_set_repeat((media_repeat_t)repeat);
        break;
    }

    case QUICK_VISUALIZER:
    {
        int viz = (int)cfg->visualizer;
        for (int i = 0; i < MEDIA_VIZ_COUNT; ++i)
        {
            viz = (viz + delta + MEDIA_VIZ_COUNT) % MEDIA_VIZ_COUNT;
            if (media_viz_available((media_viz_t)viz))
                break;
        }
        cfg->visualizer = (media_viz_t)viz;
        break;
    }

    default:
        break;
    }
}

static void quick_row_text(int row, char *label, size_t label_size, char *value, size_t value_size,
                           int *percent)
{
    const media_settings_t *cfg = media_settings();
    *percent = -1;

    switch (row)
    {
    case QUICK_VOLUME:
        snprintf(label, label_size, "Volume");
        snprintf(value, value_size, "%d%%", rg_audio_get_volume());
        *percent = rg_audio_get_volume();
        break;
    case QUICK_BRIGHTNESS:
        snprintf(label, label_size, "Brightness");
        snprintf(value, value_size, "%d%%", rg_display_get_backlight());
        *percent = rg_display_get_backlight();
        break;
    case QUICK_EQ:
        snprintf(label, label_size, "Equalizer");
        snprintf(value, value_size, "%s", cfg->eq_enabled ? media_eq_preset_name(media_eq_get_preset())
                                                          : "Off");
        break;
    case QUICK_SHUFFLE:
        snprintf(label, label_size, "Shuffle");
        snprintf(value, value_size, "%s", cfg->shuffle ? "On" : "Off");
        break;
    case QUICK_REPEAT:
        snprintf(label, label_size, "Repeat");
        snprintf(value, value_size, "%s", media_repeat_name(cfg->repeat));
        break;
    default:
        snprintf(label, label_size, "Visualizer");
        snprintf(value, value_size, "%s", media_viz_name(cfg->visualizer));
        break;
    }
}

static void quick_draw(void)
{
    media_layout_t *l = &mui.layout;
    int w = media_clampi(l->width * 3 / 4, 140, l->width - l->pad * 2);
    int h = QUICK_COUNT * (l->line_h + 4) + l->pad * 4;
    int x = (l->width - w) / 2;
    int y = (l->height - h) / 2;

    media_ui_draw_panel(x, y, w, h, media_color_scale(mui.theme.surface, 240), mui.theme.accent);

    for (int i = 0; i < QUICK_COUNT; ++i)
    {
        char label[24], value[32];
        int percent;
        quick_row_text(i, label, sizeof(label), value, sizeof(value), &percent);

        int row_y = y + l->pad * 2 + i * (l->line_h + 4);
        bool selected = i == mui.quick_row;

        if (selected)
            rg_gui_draw_rect(x + l->pad, row_y - 1, w - l->pad * 2, l->line_h + 2, 0, 0,
                             media_color_scale(mui.theme.accent, 70));

        rg_gui_draw_text(x + l->pad * 2, row_y, w / 2, label,
                         selected ? mui.theme.text : mui.theme.text_dim, C_TRANSPARENT,
                         RG_TEXT_ALIGN_LEFT);
        rg_gui_draw_text(x + w / 2, row_y, w / 2 - l->pad * 2, value,
                         selected ? mui.theme.accent : mui.theme.text_dim, C_TRANSPARENT,
                         RG_TEXT_ALIGN_RIGHT);
    }
}

/* -------------------------------------------------------------------------------------- */
/* Input                                                                                    */
/* -------------------------------------------------------------------------------------- */

static void cycle_page(int delta)
{
    int page = (int)mui.page;
    int guard = 0;

    do
    {
        page = (page + delta + MEDIA_PAGE_COUNT) % MEDIA_PAGE_COUNT;
        // The playback pages are only meaningful when something is loaded.
        if (page != MEDIA_PAGE_LIBRARY && mui.snapshot.state == MEDIA_STATE_STOPPED)
            continue;
        break;
    } while (++guard < MEDIA_PAGE_COUNT);

    mui.page = (media_page_t)page;
    mui.in_library = mui.page == MEDIA_PAGE_LIBRARY;
    mui.marquee_reset_at = mui.frame_us;
    mui.needs_redraw = true;

    if (mui.page == MEDIA_PAGE_QUEUE)
        media_ui_queue_refresh();
}

/** Accelerated seek: the longer the button is held, the bigger each step becomes. */
static int32_t seek_step_ms(int repeats)
{
    static const int32_t steps[] = {5000, 5000, 10000, 20000, 30000, 60000};
    int index = media_clampi(repeats, 0, (int)RG_COUNT(steps) - 1);
    return steps[index];
}

static void handle_seek(int direction, int repeats)
{
    int32_t delta = seek_step_ms(repeats) * direction;
    uint32_t position = mui.snapshot.position_ms;
    int64_t target = (int64_t)position + delta;

    if (target < 0)
        target = 0;
    if (mui.snapshot.duration_ms && target > (int64_t)mui.snapshot.duration_ms)
        target = mui.snapshot.duration_ms;

    media_player_seek_to((uint32_t)target);

    char value[48], now[16], total[16];
    media_format_time(now, sizeof(now), (uint32_t)target);
    media_format_time(total, sizeof(total), mui.snapshot.duration_ms);
    char delta_text[16];
    media_format_delta(delta_text, sizeof(delta_text), (int32_t)(target - (int64_t)position));
    snprintf(value, sizeof(value), "%s   %s / %s", delta_text, now, total);

    int percent = mui.snapshot.duration_ms ? (int)((target * 100) / mui.snapshot.duration_ms) : 0;
    media_ui_show_overlay("Seek", value, percent);
}

static void adjust_volume(int delta)
{
    int volume = media_clampi(rg_audio_get_volume() + delta, 0, 100);
    rg_audio_set_volume(volume);

    char value[16];
    snprintf(value, sizeof(value), "%d%%", volume);
    media_ui_show_overlay("Volume", value, volume);
}

/** Returns false when the player should exit. */
static bool handle_input(uint32_t key, int repeats)
{
    bool repeat = repeats > 0;

    /* Quick settings swallow everything while open. */
    if (mui.quick_open)
    {
        switch (key)
        {
        case RG_KEY_UP:
            mui.quick_row = (mui.quick_row + QUICK_COUNT - 1) % QUICK_COUNT;
            break;
        case RG_KEY_DOWN:
            mui.quick_row = (mui.quick_row + 1) % QUICK_COUNT;
            break;
        case RG_KEY_LEFT:
            quick_adjust(mui.quick_row, -1);
            break;
        case RG_KEY_RIGHT:
            quick_adjust(mui.quick_row, 1);
            break;
        case RG_KEY_A:
            quick_adjust(mui.quick_row, 1);
            break;
        case RG_KEY_B:
        case RG_KEY_OPTION:
            mui.quick_open = false;
            media_settings_save();
            break;
        default:
            break;
        }
        mui.needs_redraw = true;
        return true;
    }

    switch (key)
    {
    case RG_KEY_MENU:
        media_ui_player_menu();
        mui.needs_redraw = true;
        return mui.running;

    case RG_KEY_OPTION:
        mui.quick_open = true;
        mui.quick_row = QUICK_VOLUME;
        mui.needs_redraw = true;
        return true;

    case RG_KEY_SELECT:
        cycle_page(-1);
        return true;

    case RG_KEY_START:
        cycle_page(1);
        return true;

    default:
        break;
    }

    if (mui.page == MEDIA_PAGE_LIBRARY)
    {
        if (media_ui_library_input(key, repeat))
        {
            mui.needs_redraw = true;
            return true;
        }
        if (key == RG_KEY_B)
        {
            if (!media_ui_library_back())
                return false; // Leaving the top level exits the player
            mui.needs_redraw = true;
            return true;
        }
        return true;
    }

    if (mui.page == MEDIA_PAGE_QUEUE && media_ui_queue_input(key))
    {
        mui.needs_redraw = true;
        return true;
    }

    if (mui.page == MEDIA_PAGE_INFO && media_ui_info_input(key))
    {
        mui.needs_redraw = true;
        return true;
    }

    switch (key)
    {
    case RG_KEY_A:
        media_player_toggle_pause();
        break;

    case RG_KEY_B:
        // Back from a playback page returns to the library rather than exiting outright.
        mui.page = MEDIA_PAGE_LIBRARY;
        mui.in_library = true;
        break;

    case RG_KEY_LEFT:
        if (repeat)
            handle_seek(-1, repeats);
        else
            media_player_previous();
        break;

    case RG_KEY_RIGHT:
        if (repeat)
            handle_seek(1, repeats);
        else
            media_player_next();
        break;

    case RG_KEY_UP:
    case RG_KEY_DOWN:
    {
        // Unsynced lyrics have nothing to follow the transport with, so the same buttons
        // scroll them instead of touching the volume.
        const media_lyrics_t *lyrics = media_player_lyrics();
        if (mui.page == MEDIA_PAGE_LYRICS && lyrics && !lyrics->synced)
            mui.lyric_index = media_clampi(mui.lyric_index + (key == RG_KEY_UP ? -1 : 1), 0,
                                           RG_MAX(lyrics->count - 1, 0));
        else
            adjust_volume(key == RG_KEY_UP ? 5 : -5);
        break;
    }

    case RG_KEY_X:
        media_player_toggle_favorite();
        media_ui_show_overlay("Favorite", mui.snapshot.favorite ? "Removed" : "Added", -1);
        break;

    default:
        break;
    }

    mui.needs_redraw = true;
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* Frame                                                                                    */
/* -------------------------------------------------------------------------------------- */

static void draw_frame(void)
{
    media_artwork_lock();

    rg_gui_set_surface(mui.surface);
    mui.layout.mini_h = 0;

    media_ui_update_theme();
    media_ui_clear();

    switch (mui.page)
    {
    case MEDIA_PAGE_LIBRARY:     media_ui_library_draw(); break;
    case MEDIA_PAGE_NOW_PLAYING: media_ui_nowplaying_draw(); break;
    case MEDIA_PAGE_LYRICS:      media_ui_lyrics_draw(); break;
    case MEDIA_PAGE_VISUALIZER:  media_ui_visualizer_draw(); break;
    case MEDIA_PAGE_QUEUE:       media_ui_queue_draw(); break;
    case MEDIA_PAGE_INFO:        media_ui_info_draw(); break;
    default:                     media_ui_nowplaying_draw(); break;
    }

    media_ui_draw_overlay();

    if (mui.quick_open)
        quick_draw();

#if MEDIA_DEBUG_STATS
    if (media_settings()->show_debug)
    {
        char line[96];
        rg_stats_t stats = rg_system_get_stats();
        snprintf(line, sizeof(line), "%.0ffps u:%u pcm:%d%% src:%d%% int:%dk ext:%dk art:%dk",
                 (double)mui.fps, (unsigned)mui.snapshot.underruns, mui.snapshot.pcm_fill_pct,
                 mui.snapshot.src_fill_pct, stats.freeMemoryInt / 1024, stats.freeMemoryExt / 1024,
                 (int)(media_artwork_bytes_used() / 1024));
        rg_gui_draw_text(0, mui.layout.header_h, mui.layout.width, line, C_YELLOW, C_BLACK,
                         RG_TEXT_ALIGN_CENTER);
    }
#endif

    rg_gui_set_surface(NULL);
    rg_display_submit(mui.surface, 0);

    media_artwork_unlock();
}

static void on_player_event(media_event_t event, intptr_t arg, void *user)
{
    (void)arg, (void)user;

    // Anything that changes what is on screen simply asks for a redraw; the run loop owns
    // when that actually happens.
    switch (event)
    {
    case MEDIA_EVENT_TRACK_CHANGED:
        mui.marquee_reset_at = rg_system_timer();
        mui.lyric_index = -1;
        /* fallthrough */
    case MEDIA_EVENT_STATE_CHANGED:
    case MEDIA_EVENT_METADATA_READY:
    case MEDIA_EVENT_ARTWORK_READY:
    case MEDIA_EVENT_LYRICS_READY:
    case MEDIA_EVENT_ERROR:
    case MEDIA_EVENT_SD_REMOVED:
        mui.needs_redraw = true;
        break;
    default:
        break;
    }
}

void media_ui_run(void)
{
    const media_settings_t *cfg = media_settings();

    compute_layout();

    mui.surface = rg_surface_create(mui.layout.width, mui.layout.height, RG_PIXEL_565_LE, MEM_SLOW);
    if (!mui.surface)
    {
        rg_gui_alert("Media Player", "Not enough memory to start.");
        return;
    }

    mui.running = true;
    mui.needs_redraw = true;
    mui.lyric_index = -1;
    mui.overlay_percent = -1;
    mui.marquee_reset_at = rg_system_timer();

    media_player_set_event_callback(&on_player_event, NULL);

    // Opening straight into Now Playing when music is already going is what a portable
    // player does; otherwise the library is the only useful place to be.
    if (media_player_active() && cfg->default_page != MEDIA_PAGE_LIBRARY)
        mui.page = cfg->default_page;
    else
        mui.page = MEDIA_PAGE_LIBRARY;
    mui.in_library = mui.page == MEDIA_PAGE_LIBRARY;

    media_ui_library_enter(MEDIA_BROWSE_HOME, 0, NULL);

    // A launcher shortcut asked for a specific view; honour it now that the browser exists.
    if (pending_view > MEDIA_BROWSE_HOME && pending_view < MEDIA_BROWSE_COUNT)
    {
        media_ui_library_enter((media_browse_mode_t)pending_view, 0, NULL);
        mui.page = MEDIA_PAGE_LIBRARY;
        mui.in_library = true;
    }
    pending_view = -1;

    uint32_t previous_keys = 0;
    int64_t next_repeat = 0;
    int repeats = 0;
    int64_t next_frame = 0;
    int64_t next_fft = 0;
    int64_t fps_window = rg_system_timer();
    uint32_t fps_frames = 0;

    const int64_t frame_interval = 1000000 / media_clampi(cfg->visualizer_fps, 10, 60);
    const int64_t fft_interval = 1000000 / 25;

    while (mui.running)
    {
        mui.frame_us = rg_system_timer();

        /* --- Input ------------------------------------------------------------------- */
        uint32_t keys = rg_input_read_gamepad();
        uint32_t pressed = 0;

        if (keys)
        {
            if (keys != previous_keys)
            {
                pressed = keys;
                repeats = 0;
                next_repeat = mui.frame_us + REPEAT_DELAY_US;
            }
            else if (mui.frame_us >= next_repeat)
            {
                pressed = keys;
                repeats++;
                // Accelerate while held, but never faster than ~8 Hz
                next_repeat = mui.frame_us + RG_MAX(REPEAT_DELAY_US / (repeats + 1), 120000);
            }
        }
        else
        {
            repeats = 0;
        }
        previous_keys = keys;

        /* --- State ------------------------------------------------------------------- */
        media_player_tick();
        mui.snapshot = media_player_snapshot();
        mui.track = media_player_track();

        if (pressed)
        {
            // Isolate a single logical key so chords do not trigger two actions at once.
            uint32_t key = pressed & (RG_KEY_MENU | RG_KEY_OPTION);
            if (!key)
                key = pressed & (RG_KEY_SELECT | RG_KEY_START);
            if (!key)
                key = pressed & (RG_KEY_A | RG_KEY_B | RG_KEY_X | RG_KEY_Y);
            if (!key)
                key = pressed & (RG_KEY_UP | RG_KEY_DOWN | RG_KEY_LEFT | RG_KEY_RIGHT);

            if (key && !handle_input(key, repeats))
                break;
        }

        /* --- Visualiser -------------------------------------------------------------- */
        if (mui.frame_us >= next_fft)
        {
            next_fft = mui.frame_us + fft_interval;
            if (media_fft_ready() && mui.snapshot.state == MEDIA_STATE_PLAYING)
            {
                media_fft_analyze();
                if (mui.page == MEDIA_PAGE_VISUALIZER || mui.page == MEDIA_PAGE_NOW_PLAYING)
                    mui.needs_redraw = true;
            }
        }

        /* --- Render ------------------------------------------------------------------ */
        bool animating = media_anim_running(&mui.progress_anim) ||
                         mui.frame_us < mui.overlay_until_us ||
                         mui.snapshot.state == MEDIA_STATE_PLAYING ||
                         mui.snapshot.state == MEDIA_STATE_BUFFERING;

        if ((mui.needs_redraw || animating) && mui.frame_us >= next_frame)
        {
            next_frame = mui.frame_us + frame_interval;
            mui.needs_redraw = false;
            draw_frame();
            mui.frames++;
            fps_frames++;

            if (mui.frame_us - fps_window >= 1000000)
            {
                mui.fps = (float)fps_frames * 1000000.0f / (float)(mui.frame_us - fps_window);
                fps_window = mui.frame_us;
                fps_frames = 0;
            }
        }
        else
        {
            // Idle: let the decode, IO and scan tasks have the core.
            rg_task_delay(10);
        }
    }

    // "Open on the last page used" only means anything if we record it.
    if (mui.page != MEDIA_PAGE_LIBRARY)
        media_settings()->default_page = mui.page;

    media_player_set_event_callback(NULL, NULL);
    media_list_free(&mui.list);
    rg_surface_free(mui.surface);
    mui.surface = NULL;
    mui.running = false;
}
