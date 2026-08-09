#include <rg_system.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "media_audio.h"
#include "media_fft.h"
#include "media_ui_internal.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_UI"

/* -------------------------------------------------------------------------------------- */
/* Shared pieces                                                                            */
/* -------------------------------------------------------------------------------------- */

static const char *track_title(void)
{
    if (mui.track && mui.track->title[0])
        return mui.track->title;
    const char *path = media_player_path();
    return path ? rg_basename(path) : "Nothing playing";
}

static const char *track_artist(void)
{
    if (mui.track && mui.track->artist[0])
        return mui.track->artist;
    return "Unknown Artist";
}

static const char *track_album(void)
{
    if (mui.track && mui.track->album[0])
        return mui.track->album;
    return "Unknown Album";
}

static int position_percent(void)
{
    if (!mui.snapshot.duration_ms)
        return 0;
    return (int)(((uint64_t)mui.snapshot.position_ms * 100) / mui.snapshot.duration_ms);
}

/** State/shuffle/repeat/volume strip shared by the playback pages. */
static void draw_status_row(int x, int y, int w)
{
    char left[48];
    char right[48];

    const char *state = "";
    switch (mui.snapshot.state)
    {
    case MEDIA_STATE_PLAYING:   state = "Playing"; break;
    case MEDIA_STATE_PAUSED:    state = "Paused"; break;
    case MEDIA_STATE_BUFFERING: state = "Buffering..."; break;
    case MEDIA_STATE_LOADING:   state = "Loading..."; break;
    case MEDIA_STATE_SEEKING:   state = "Seeking..."; break;
    case MEDIA_STATE_ENDED:     state = "End of queue"; break;
    case MEDIA_STATE_ERROR:     state = media_player_last_error() ?: "Error"; break;
    default:                    state = "Stopped"; break;
    }

    snprintf(left, sizeof(left), "%s%s%s", state, mui.snapshot.shuffle ? "  SHUF" : "",
             mui.snapshot.repeat != MEDIA_REPEAT_OFF ? "  RPT" : "");

    if (mui.snapshot.repeat == MEDIA_REPEAT_TRACK)
        snprintf(left, sizeof(left), "%s%s  RPT1", state, mui.snapshot.shuffle ? "  SHUF" : "");

    snprintf(right, sizeof(right), "%s  %d%%", mui.snapshot.favorite ? "FAV" : "",
             mui.snapshot.volume);

    rg_gui_draw_text(x, y, w / 2, left, mui.theme.text_dim, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(x + w / 2, y, w / 2, right, mui.theme.accent, C_TRANSPARENT,
                     RG_TEXT_ALIGN_RIGHT);
}

/** Transport row: previous / play-pause / next, drawn as shapes. */
static void draw_transport(int cx, int y, int size)
{
    rg_color_t color = mui.theme.text;
    int gap = size * 2;

    // Previous
    for (int i = 0; i < size; ++i)
        rg_gui_draw_rect(cx - gap - size + i, y + i / 2, 1, size - i, 0, 0, color);
    rg_gui_draw_rect(cx - gap - size - 2, y, 2, size, 0, 0, color);

    // Play / pause
    if (mui.snapshot.state == MEDIA_STATE_PLAYING || mui.snapshot.state == MEDIA_STATE_BUFFERING)
    {
        rg_gui_draw_rect(cx - size / 2 - 1, y - 2, size / 3 + 1, size + 4, 0, 0, mui.theme.accent);
        rg_gui_draw_rect(cx + size / 6, y - 2, size / 3 + 1, size + 4, 0, 0, mui.theme.accent);
    }
    else
    {
        for (int i = 0; i < size; ++i)
            rg_gui_draw_rect(cx - size / 2, y + i / 2, size - i, 1, 0, 0, mui.theme.accent);
    }

    // Next
    for (int i = 0; i < size; ++i)
        rg_gui_draw_rect(cx + gap + i, y + i / 2, 1, size - i, 0, 0, color);
    rg_gui_draw_rect(cx + gap + size, y, 2, size, 0, 0, color);
}

/* -------------------------------------------------------------------------------------- */
/* Now Playing                                                                              */
/* -------------------------------------------------------------------------------------- */

void media_ui_nowplaying_draw(void)
{
    media_layout_t *l = &mui.layout;

    if (mui.snapshot.state == MEDIA_STATE_STOPPED && !mui.track)
    {
        media_ui_draw_header("Now Playing", NULL);
        media_ui_draw_message("Nothing playing", "Pick something from the library.");
        media_ui_draw_footer("B: Library   MENU: Options");
        return;
    }

    char elapsed[16], total[16];
    media_format_time(elapsed, sizeof(elapsed), mui.snapshot.position_ms);
    media_format_time(total, sizeof(total), mui.snapshot.duration_ms);

    char clock_text[16] = "";
    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now))
        snprintf(clock_text, sizeof(clock_text), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);

    media_ui_draw_header("Now Playing", clock_text);

    // Layout: art on the left for wide screens, centred above the text for narrow ones.
    bool wide = l->width >= l->height * 5 / 4 && l->width >= 400;
    media_palette_t palette = media_artwork_palette(media_player_path());

    int content_bottom = l->height - l->footer_h - l->pad;
    int art_size;
    int text_x, text_w, text_y;

    if (wide)
    {
        art_size = media_clampi(l->content_h - l->pad * 4, 48, l->height / 2 + l->height / 6);
        media_ui_draw_art(l->pad * 2, l->content_top + l->pad * 2, art_size, media_player_path(),
                          &palette, NULL);
        text_x = l->pad * 3 + art_size;
        text_w = l->width - text_x - l->pad * 2;
        text_y = l->content_top + l->pad * 2;
    }
    else
    {
        art_size = media_clampi(l->content_h / 2, 40, l->width / 2);
        media_ui_draw_art((l->width - art_size) / 2, l->content_top + l->pad, art_size,
                          media_player_path(), &palette, NULL);
        text_x = l->pad * 2;
        text_w = l->width - l->pad * 4;
        text_y = l->content_top + l->pad * 2 + art_size;
    }

    media_ui_draw_marquee(text_x, text_y, text_w, track_title(), mui.theme.text, RG_TEXT_BIGGER,
                          true);
    text_y += l->line_h + 2;
    media_ui_draw_marquee(text_x, text_y, text_w, track_artist(), mui.theme.accent, 0, true);
    text_y += l->line_h;
    media_ui_draw_marquee(text_x, text_y, text_w, track_album(), mui.theme.text_dim, 0, false);

    /* Progress */
    int bar_y = content_bottom - l->line_h * 3 - l->pad;
    int bar_x = l->pad * 2;
    int bar_w = l->width - l->pad * 4;

    // Interpolated only between position updates; it never runs ahead of the real position.
    int percent = position_percent();
    media_ui_draw_progress(bar_x, bar_y, bar_w, 3, percent, mui.theme.accent, mui.theme.divider);

    rg_gui_draw_text(bar_x, bar_y + 6, bar_w / 2, elapsed, mui.theme.text_dim, C_TRANSPARENT,
                     RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(bar_x + bar_w / 2, bar_y + 6, bar_w / 2, total, mui.theme.text_dim,
                     C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);

    /* Transport */
    draw_transport(l->width / 2, bar_y + 6 + l->line_h + l->pad, RG_MAX(l->line_h / 2, 5));

    /* Quality + next track */
    char quality[64] = "";
    if (mui.snapshot.sample_rate)
    {
        if (mui.snapshot.bitrate)
            snprintf(quality, sizeof(quality), "%s %ukbps %.1fkHz",
                     media_codec_name((media_codec_t)mui.snapshot.codec),
                     (unsigned)(mui.snapshot.bitrate / 1000), mui.snapshot.sample_rate / 1000.0);
        else
            snprintf(quality, sizeof(quality), "%s %.1fkHz",
                     media_codec_name((media_codec_t)mui.snapshot.codec),
                     mui.snapshot.sample_rate / 1000.0);
    }

    draw_status_row(l->pad * 2, l->content_top + l->pad / 2, l->width - l->pad * 4);

    if (quality[0])
        rg_gui_draw_text(l->pad * 2, bar_y - l->line_h - 2, l->width - l->pad * 4, quality,
                         mui.theme.divider, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);

    // Resolving the next track means reading a record off the card, so the result is cached
    // against its id rather than fetched on every frame.
    static uint32_t next_cached_id;
    static char next_line[96];

    if (mui.snapshot.next_track_id != next_cached_id)
    {
        next_cached_id = mui.snapshot.next_track_id;
        next_line[0] = 0;

        media_track_t next;
        if (next_cached_id && media_library_get_track(next_cached_id, &next))
            snprintf(next_line, sizeof(next_line), "Next: %.32s - %.24s", next.title,
                     next.artist[0] ? next.artist : "Unknown");
    }

    if (next_line[0])
        rg_gui_draw_text(l->pad * 2, bar_y - l->line_h - 2, (l->width - l->pad * 4) * 2 / 3,
                         next_line, mui.theme.divider, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);

    media_ui_draw_footer("A: Play/Pause   LEFT/RIGHT: Track (hold: seek)   UP/DOWN: Volume");
}

/* -------------------------------------------------------------------------------------- */
/* Lyrics                                                                                   */
/* -------------------------------------------------------------------------------------- */

void media_ui_lyrics_draw(void)
{
    media_layout_t *l = &mui.layout;
    const media_lyrics_t *lyrics = media_player_lyrics();

    media_ui_draw_header("Lyrics", track_title());

    if (!media_settings()->lyrics_enabled)
    {
        media_ui_draw_message("Lyrics are off", "Enable them in the player menu.");
        media_ui_draw_footer("MENU: Options");
        return;
    }

    if (!lyrics)
    {
        media_ui_draw_message("No lyrics", "Place a .lrc file beside the track,\n"
                                           "or embed them in its tags.");
        media_ui_draw_footer("B: Back   MENU: Options");
        return;
    }

    int32_t offset = media_settings()->lyrics_offset_ms;
    int current = lyrics->synced ? media_lyrics_find(lyrics, mui.snapshot.position_ms, offset)
                                 : mui.lyric_index;

    if (!lyrics->synced)
    {
        // Unsynced text simply scrolls with the transport buttons.
        current = media_clampi(mui.lyric_index, 0, RG_MAX(lyrics->count - 1, 0));
    }
    else if (current != mui.lyric_index)
    {
        // Animate the jump so the active line glides rather than snapping.
        media_anim_start(&mui.lyric_anim, mui.lyric_index * 100, current * 100, 220);
        mui.lyric_index = current;
    }

    int row_h = l->line_h + 3;
    int rows = RG_MAX((l->content_h - l->pad * 2) / row_h, 3);
    int focus_row = rows / 2;

    // Fractional scroll position, so the block moves smoothly between lines.
    int32_t animated = media_anim_value(&mui.lyric_anim, mui.frame_us);
    float centre = lyrics->synced ? (float)animated / 100.0f : (float)current;

    int top = l->content_top + l->pad;

    for (int i = 0; i < rows; ++i)
    {
        int index = (int)lroundf(centre) + i - focus_row;
        if (index < 0 || index >= lyrics->count)
            continue;

        const char *text = media_lyrics_text(lyrics, index);
        if (!text || !*text)
            continue;

        int distance = abs(index - current);
        rg_color_t color;

        if (index == current)
            color = mui.theme.accent;
        else if (distance == 1)
            color = mui.theme.text;
        else if (distance == 2)
            color = mui.theme.text_dim;
        else
            color = mui.theme.divider;

        int y = top + i * row_h;
        // A subtle sub-line offset from the animation keeps the motion continuous.
        if (lyrics->synced)
            y -= (int)((centre - lroundf(centre)) * row_h);

        if (y < l->content_top || y + l->line_h > l->height - l->footer_h)
            continue;

        rg_gui_draw_text(l->pad * 2, y, l->width - l->pad * 4, text, color, C_TRANSPARENT,
                         (index == current ? RG_TEXT_BIGGER : 0) | RG_TEXT_ALIGN_CENTER);
    }

    char footer[64];
    if (offset)
        snprintf(footer, sizeof(footer), "Offset %+dms   MENU: Options", (int)offset);
    else
        snprintf(footer, sizeof(footer), "A: Play/Pause   MENU: Lyrics Offset");
    media_ui_draw_footer(footer);
}

/* -------------------------------------------------------------------------------------- */
/* Visualiser                                                                               */
/* -------------------------------------------------------------------------------------- */

static void viz_bars(int x, int y, int w, int h, bool mirrored)
{
    const media_spectrum_t *spectrum = media_fft_spectrum();
    int bands = spectrum->bands ? spectrum->bands : 1;
    int gap = RG_MAX(w / (bands * 8), 1);
    int bar_w = RG_MAX((w - gap * (bands - 1)) / bands, 1);

    for (int i = 0; i < bands; ++i)
    {
        int bx = x + i * (bar_w + gap);
        float value = media_clampf(spectrum->value[i], 0.0f, 1.0f);
        float peak = media_clampf(spectrum->peak[i], 0.0f, 1.0f);

        // Colour ramps across the spectrum so the bands stay distinguishable in mono vision.
        rg_color_t color = media_color_blend(mui.theme.accent, mui.theme.highlight,
                                             (i * 255) / bands);

        if (mirrored)
        {
            int half = h / 2;
            int bh = (int)(value * half);
            rg_gui_draw_rect(bx, y + half - bh, bar_w, bh, 0, 0, color);
            rg_gui_draw_rect(bx, y + half, bar_w, bh, 0, 0, media_color_scale(color, 140));
            int py = y + half - (int)(peak * half);
            rg_gui_draw_rect(bx, py, bar_w, 1, 0, 0, mui.theme.text);
        }
        else
        {
            int bh = (int)(value * h);
            rg_gui_draw_rect(bx, y + h - bh, bar_w, bh, 0, 0, color);
            int py = y + h - (int)(peak * h);
            rg_gui_draw_rect(bx, media_clampi(py, y, y + h - 1), bar_w, 1, 0, 0, mui.theme.text);
        }
    }
}

static void viz_waveform(int x, int y, int w, int h, bool filled)
{
    int16_t *samples = malloc((size_t)w * sizeof(int16_t));
    if (!samples)
        return;

    size_t count = media_fft_copy_waveform(samples, (size_t)w);
    int mid = y + h / 2;

    for (size_t i = 0; i < count; ++i)
    {
        int value = (samples[i] * (h / 2)) / 32768;
        if (filled)
        {
            int top = value >= 0 ? mid - value : mid;
            int height = abs(value) + 1;
            rg_gui_draw_rect(x + (int)i, top, 1, height, 0, 0, mui.theme.accent);
        }
        else
        {
            rg_gui_draw_rect(x + (int)i, media_clampi(mid - value, y, y + h - 1), 1, 2, 0, 0,
                             mui.theme.accent);
        }
    }

    free(samples);
}

static void viz_circular(int cx, int cy, int radius)
{
    const media_spectrum_t *spectrum = media_fft_spectrum();
    int bands = spectrum->bands ? spectrum->bands : 1;

    for (int i = 0; i < bands; ++i)
    {
        float angle = (float)i * 2.0f * (float)M_PI / bands;
        float value = media_clampf(spectrum->value[i], 0.0f, 1.0f);
        int length = (int)(value * radius * 0.7f) + 2;

        rg_color_t color = media_color_blend(mui.theme.accent, mui.theme.highlight,
                                             (i * 255) / bands);

        // Sampled line: a handful of points is plenty at this radius and avoids a full
        // Bresenham implementation in the hot path.
        for (int s = 0; s < length; ++s)
        {
            int px = cx + (int)(cosf(angle) * (radius * 0.35f + s));
            int py = cy + (int)(sinf(angle) * (radius * 0.35f + s));
            rg_gui_draw_rect(px, py, 2, 2, 0, 0, color);
        }
    }
}

static void viz_meters(int x, int y, int w, int h, bool peak_mode)
{
    const media_spectrum_t *spectrum = media_fft_spectrum();
    int bar_h = RG_MAX(h / 4, 6);
    int gap = bar_h / 2;

    struct
    {
        const char *label;
        float rms, peak;
    } channels[2] = {
        {"L", spectrum->rms_left, spectrum->peak_left},
        {"R", spectrum->rms_right, spectrum->peak_right},
    };

    for (int i = 0; i < 2; ++i)
    {
        int by = y + h / 2 - bar_h - gap / 2 + i * (bar_h + gap);
        rg_gui_draw_text(x, by, 16, channels[i].label, mui.theme.text_dim, C_TRANSPARENT,
                         RG_TEXT_ALIGN_LEFT);

        int bar_x = x + 20;
        int bar_w = w - 24;
        rg_gui_draw_rect(bar_x, by, bar_w, bar_h, 0, 0, mui.theme.divider);

        int rms_w = (int)(media_clampf(channels[i].rms * 2.5f, 0.0f, 1.0f) * bar_w);
        rg_gui_draw_rect(bar_x, by, rms_w, bar_h, 0, 0, mui.theme.accent);

        if (peak_mode)
        {
            int peak_x = bar_x + (int)(media_clampf(channels[i].peak, 0.0f, 1.0f) * bar_w);
            rg_gui_draw_rect(media_clampi(peak_x, bar_x, bar_x + bar_w - 2), by, 2, bar_h, 0, 0,
                             C_RED);
        }
    }
}

void media_ui_visualizer_draw(void)
{
    media_layout_t *l = &mui.layout;
    const media_settings_t *cfg = media_settings();

    media_ui_draw_header(media_viz_name(cfg->visualizer), track_title());

    int x = l->pad * 2;
    int y = l->content_top + l->pad * 2;
    int w = l->width - l->pad * 4;
    int h = l->content_h - l->pad * 6 - l->line_h;

    if (h < 16)
        h = 16;

    if (!media_fft_ready())
    {
        media_ui_draw_message("Visualizer unavailable", "There was not enough memory for the FFT.");
        media_ui_draw_footer("MENU: Options");
        return;
    }

    media_palette_t palette = media_artwork_palette(media_player_path());

    switch (cfg->visualizer)
    {
    case MEDIA_VIZ_MIRRORED:
        viz_bars(x, y, w, h, true);
        break;

    case MEDIA_VIZ_WAVEFORM:
        viz_waveform(x, y, w, h, true);
        break;

    case MEDIA_VIZ_OSCILLOSCOPE:
        viz_waveform(x, y, w, h, false);
        break;

    case MEDIA_VIZ_CIRCULAR:
        viz_circular(l->width / 2, y + h / 2, RG_MIN(w, h) / 2);
        break;

    case MEDIA_VIZ_VU:
        viz_meters(x, y, w, h, false);
        break;

    case MEDIA_VIZ_PEAK:
        viz_meters(x, y, w, h, true);
        break;

    case MEDIA_VIZ_PARTICLES:
    {
        // Bars plus a sparse spray of points driven by the same band values: the "particle"
        // budget is fixed, so the cost cannot run away with a busy track.
        const media_spectrum_t *spectrum = media_fft_spectrum();
        viz_bars(x, y, w, h, false);
        for (int i = 0; i < spectrum->bands; ++i)
        {
            float value = spectrum->value[i];
            int count = (int)(value * 4);
            for (int p = 0; p < count; ++p)
            {
                int px = x + (w * i) / spectrum->bands + (int)((mui.frame_us / 1000 + p * 37) % 9);
                int py = y + h - (int)(value * h) - p * 4 -
                         (int)((mui.frame_us / 40000 + p) % 6);
                if (py > y && py < y + h)
                    rg_gui_draw_rect(px, py, 2, 2, 0, 0, mui.theme.highlight);
            }
        }
        break;
    }

    case MEDIA_VIZ_ART_PULSE:
    case MEDIA_VIZ_VINYL:
    {
        const media_spectrum_t *spectrum = media_fft_spectrum();
        float level = media_clampf(spectrum->rms_left + spectrum->rms_right, 0.0f, 1.0f);
        int base = RG_MIN(w, h);
        int size = base - (int)((1.0f - level) * base / 8);
        size = media_clampi(size, 16, base);

        media_ui_draw_art(l->width / 2 - size / 2, y + (h - size) / 2, size, media_player_path(),
                          &palette, NULL);

        if (cfg->visualizer == MEDIA_VIZ_VINYL)
        {
            // Spindle hole, so the pulsing cover reads as a record
            int hole = RG_MAX(size / 12, 4);
            rg_gui_draw_rect(l->width / 2 - hole / 2, y + (h - hole) / 2 + 0, hole, hole, 0, 0,
                             mui.theme.background);
        }
        break;
    }

    case MEDIA_VIZ_CLOCK:
    {
        char clock_text[16] = "--:--";
        time_t now = time(NULL);
        struct tm tm_now;
        if (localtime_r(&now, &tm_now))
            snprintf(clock_text, sizeof(clock_text), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);

        rg_gui_draw_text(0, y + h / 2 - l->line_h, l->width, clock_text, mui.theme.text,
                         C_TRANSPARENT, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
        rg_gui_draw_text(0, y + h / 2 + l->line_h / 2, l->width, track_title(), mui.theme.accent,
                         C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
        rg_gui_draw_text(0, y + h / 2 + l->line_h * 3 / 2, l->width, track_artist(),
                         mui.theme.text_dim, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
        break;
    }

    case MEDIA_VIZ_MINIMAL:
    {
        int size = media_clampi(RG_MIN(w, h) - l->line_h * 2, 32, RG_MIN(w, h));
        media_ui_draw_art(l->width / 2 - size / 2, y, size, media_player_path(), &palette, NULL);
        viz_bars(x, y + size + 2, w, RG_MAX(h - size - 4, 6), false);
        break;
    }

    default:
        viz_bars(x, y, w, h, false);
        break;
    }

    /* Progress and title strip under every mode */
    int bar_y = l->height - l->footer_h - l->line_h - l->pad;
    media_ui_draw_progress(x, bar_y, w, 2, position_percent(), mui.theme.accent, mui.theme.divider);

    char times[48], elapsed[16], total[16];
    media_format_time(elapsed, sizeof(elapsed), mui.snapshot.position_ms);
    media_format_time(total, sizeof(total), mui.snapshot.duration_ms);
    snprintf(times, sizeof(times), "%s / %s", elapsed, total);
    rg_gui_draw_text(x, bar_y + 4, w, times, mui.theme.text_dim, C_TRANSPARENT,
                     RG_TEXT_ALIGN_RIGHT);
    media_ui_draw_marquee(x, bar_y + 4, w * 2 / 3, track_artist(), mui.theme.text_dim, 0, false);

    media_ui_draw_footer("MENU: Visualizer   START/SELECT: Page");
}

/* -------------------------------------------------------------------------------------- */
/* Queue                                                                                    */
/* -------------------------------------------------------------------------------------- */

void media_ui_queue_refresh(void)
{
    media_list_reset(&mui.list);

    // Every resolved row costs one record read. Past a few hundred entries the wait becomes
    // noticeable, so longer queues fall back to filenames, which are still perfectly usable.
    const int resolve_limit = 400;

    int count = media_queue_count();
    for (int i = 0; i < count; ++i)
    {
        const char *path = media_queue_path(i);
        if (!path)
            continue;

        media_list_item_t *item = media_list_add(&mui.list);
        if (!item)
            break;

        uint32_t id = media_queue_id(i);
        media_track_t track;

        if (id && i < resolve_limit && media_library_get_track(id, &track))
        {
            snprintf(item->text, sizeof(item->text), "%s", track.title);
            snprintf(item->subtext, sizeof(item->subtext), "%.24s",
                     track.artist[0] ? track.artist : "Unknown Artist");
        }
        else
        {
            media_path_stem(item->text, sizeof(item->text), path);
            media_utf8_copy(item->subtext, sizeof(item->subtext), rg_basename(rg_dirname(path)));
        }

        item->kind = 2;
        item->arg = (uint32_t)i;
    }

    mui.list.cursor = media_clampi(media_queue_index(), 0, RG_MAX(mui.list.count - 1, 0));
    media_list_move(&mui.list, 0, 0);
}

bool media_ui_queue_input(uint32_t key)
{
    switch (key)
    {
    case RG_KEY_UP:
        media_list_move(&mui.list, -1, 0);
        return true;
    case RG_KEY_DOWN:
        media_list_move(&mui.list, 1, 0);
        return true;
    case RG_KEY_LEFT:
        media_list_move(&mui.list, -1, 1);
        return true;
    case RG_KEY_RIGHT:
        media_list_move(&mui.list, 1, 1);
        return true;
    case RG_KEY_A:
        if (mui.list.cursor < mui.list.count)
        {
            media_player_play_index((int)mui.list.items[mui.list.cursor].arg);
            mui.page = MEDIA_PAGE_NOW_PLAYING;
        }
        return true;
    case RG_KEY_Y:
        if (mui.list.cursor < mui.list.count)
        {
            media_queue_remove((int)mui.list.items[mui.list.cursor].arg);
            media_ui_queue_refresh();
        }
        return true;
    default:
        return false;
    }
}

void media_ui_queue_draw(void)
{
    media_layout_t *l = &mui.layout;
    char right[24];

    snprintf(right, sizeof(right), "%d tracks", media_queue_count());
    media_ui_draw_header("Queue", right);

    if (mui.list.count == 0)
    {
        media_ui_draw_message("Queue is empty", "Play something from the library.");
        media_ui_draw_footer("B: Back");
        return;
    }

    int rows = media_list_visible_rows();
    int row_h = l->line_h + 2;
    int top = l->content_top + l->pad / 2;
    int list_w = l->width - l->pad * 3;
    int current = media_queue_index();

    for (int i = 0; i < rows; ++i)
    {
        int index = mui.list.scroll + i;
        if (index >= mui.list.count)
            break;

        const media_list_item_t *item = &mui.list.items[index];
        bool selected = index == mui.list.cursor;
        bool playing = (int)item->arg == current;
        int y = top + i * row_h;

        if (selected)
            media_ui_draw_panel(l->pad, y - 1, list_w, row_h, media_color_scale(mui.theme.accent, 60),
                                mui.theme.accent);

        char line[MEDIA_LIST_TEXT + 8];
        snprintf(line, sizeof(line), "%s%s", playing ? "> " : "  ", item->text);

        rg_gui_draw_text(l->pad * 2, y, list_w - l->pad * 2 - 60, line,
                         playing ? mui.theme.accent : (selected ? mui.theme.text : mui.theme.text_dim),
                         C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
        rg_gui_draw_text(l->pad + list_w - 60, y, 56, item->subtext, mui.theme.divider,
                         C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
    }

    media_ui_draw_scrollbar(l->width - l->pad, top, rows * row_h, rows, mui.list.count,
                            mui.list.scroll);
    media_ui_draw_footer("A: Play   Y: Remove   MENU: Options");
}

/* -------------------------------------------------------------------------------------- */
/* Track information                                                                        */
/* -------------------------------------------------------------------------------------- */

static int info_scroll;

bool media_ui_info_input(uint32_t key)
{
    if (key == RG_KEY_UP)
    {
        info_scroll = RG_MAX(info_scroll - 1, 0);
        return true;
    }
    if (key == RG_KEY_DOWN)
    {
        info_scroll++;
        return true;
    }
    return false;
}

void media_ui_info_draw(void)
{
    media_layout_t *l = &mui.layout;

    media_ui_draw_header("Track Info", NULL);

    if (!mui.track)
    {
        media_ui_draw_message("No track", "Nothing is loaded.");
        media_ui_draw_footer("B: Back");
        return;
    }

    const media_track_t *t = mui.track;

    char duration[16], size[16], samplerate[16], bitrate[16], channels[16], bits[16], year[16];
    char track_no[16], disc_no[16];

    media_format_time(duration, sizeof(duration), t->duration_ms);
    media_format_size(size, sizeof(size), t->file_size);
    snprintf(samplerate, sizeof(samplerate), "%u Hz", (unsigned)t->sample_rate);
    if (t->bitrate)
        snprintf(bitrate, sizeof(bitrate), "%u kbps", (unsigned)(t->bitrate / 1000));
    else
        snprintf(bitrate, sizeof(bitrate), "-");
    snprintf(channels, sizeof(channels), "%u", t->channels);
    snprintf(bits, sizeof(bits), "%u bit", t->bits_per_sample);
    snprintf(year, sizeof(year), "%u", t->year);
    snprintf(track_no, sizeof(track_no), "%u", t->track_number);
    snprintf(disc_no, sizeof(disc_no), "%u", t->disc_number);

    struct
    {
        const char *label;
        const char *value;
    } fields[] = {
        {"Title",        t->title},
        {"Artist",       t->artist[0] ? t->artist : "-"},
        {"Album",        t->album[0] ? t->album : "-"},
        {"Album Artist", t->album_artist[0] ? t->album_artist : "-"},
        {"Genre",        t->genre[0] ? t->genre : "-"},
        {"Year",         t->year ? year : "-"},
        {"Track",        t->track_number ? track_no : "-"},
        {"Disc",         t->disc_number ? disc_no : "-"},
        {"Codec",        media_codec_name((media_codec_t)t->codec)},
        {"Bitrate",      bitrate},
        {"Sample Rate",  samplerate},
        {"Bit Depth",    bits},
        {"Channels",     channels},
        {"Duration",     duration},
        {"File Size",    size},
        {"Lyrics",       media_player_lyrics_available() ? "Available" : "-"},
        {"Artwork",      t->has_embedded_art ? "Embedded" : "Folder / none"},
        {"Path",         t->path},
    };

    int row_h = l->line_h + 2;
    int rows = RG_MAX((l->content_h - l->pad) / row_h, 1);
    int max_scroll = RG_MAX((int)RG_COUNT(fields) - rows, 0);
    info_scroll = media_clampi(info_scroll, 0, max_scroll);

    for (int i = 0; i < rows; ++i)
    {
        int index = info_scroll + i;
        if (index >= (int)RG_COUNT(fields))
            break;

        int y = l->content_top + l->pad / 2 + i * row_h;
        rg_gui_draw_text(l->pad * 2, y, l->width / 3, fields[index].label, mui.theme.text_dim,
                         C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
        media_ui_draw_marquee(l->pad * 2 + l->width / 3, y, l->width - l->width / 3 - l->pad * 4,
                              fields[index].value, mui.theme.text, 0, i == 0);
    }

    media_ui_draw_scrollbar(l->width - l->pad, l->content_top, rows * row_h, rows,
                            (int)RG_COUNT(fields), info_scroll);
    media_ui_draw_footer("UP/DOWN: Scroll   B: Back");
}
