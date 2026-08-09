#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media.h"
#include "media_audio.h"
#include "media_fft.h"
#include "media_metadata.h"
#include "media_playlist.h"
#include "media_ui_internal.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_UI"

/* -------------------------------------------------------------------------------------- */
/* Small option helpers                                                                     */
/* -------------------------------------------------------------------------------------- */

static int cycle(int value, int count, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV && --value < 0)
        value = count - 1;
    if ((event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER) && ++value >= count)
        value = 0;
    return value;
}

static rg_gui_event_t shuffle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        cfg->shuffle = !cfg->shuffle;
        media_player_set_shuffle(cfg->shuffle);
    }
    strcpy(option->value, cfg->shuffle ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t repeat_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        cfg->repeat = (media_repeat_t)cycle((int)cfg->repeat, MEDIA_REPEAT_COUNT, event);
        media_player_set_repeat(cfg->repeat);
    }
    strcpy(option->value, media_repeat_name(cfg->repeat));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t lyrics_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->lyrics_enabled = !cfg->lyrics_enabled;
    strcpy(option->value, cfg->lyrics_enabled ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t lyrics_offset_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV)
        cfg->lyrics_offset_ms = media_clampi(cfg->lyrics_offset_ms - 250, -10000, 10000);
    if (event == RG_DIALOG_NEXT)
        cfg->lyrics_offset_ms = media_clampi(cfg->lyrics_offset_ms + 250, -10000, 10000);
    if (event == RG_DIALOG_ENTER)
        cfg->lyrics_offset_ms = 0;
    sprintf(option->value, "%+d ms", (int)cfg->lyrics_offset_ms);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t normalize_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *names[] = {"Off", "Track", "Album"};
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->normalization = (media_normalize_t)cycle((int)cfg->normalization,
                                                      MEDIA_NORMALIZE_COUNT, event);
    strcpy(option->value, names[cfg->normalization % MEDIA_NORMALIZE_COUNT]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t gapless_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->gapless = !cfg->gapless;
    strcpy(option->value, cfg->gapless ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t crossfade_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const int values[] = {0, 1, 2, 3, 5};
    media_settings_t *cfg = media_settings();

    if (!media_profile()->crossfade_allowed)
    {
        option->flags = RG_DIALOG_FLAG_DISABLED;
        strcpy(option->value, "N/A");
        return RG_DIALOG_VOID;
    }

    int index = 0;
    for (size_t i = 0; i < RG_COUNT(values); ++i)
    {
        if (values[i] == cfg->crossfade_s)
            index = (int)i;
    }
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->crossfade_s = values[cycle(index, (int)RG_COUNT(values), event)];

    if (cfg->crossfade_s)
        sprintf(option->value, "%d s", cfg->crossfade_s);
    else
        strcpy(option->value, "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t background_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *names[] = {"Off", "Launcher only", "Always"};
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->background_playback = (media_background_t)cycle((int)cfg->background_playback,
                                                             MEDIA_BACKGROUND_COUNT, event);
    strcpy(option->value, names[cfg->background_playback % MEDIA_BACKGROUND_COUNT]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t resume_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *names[] = {"Off", "Last track", "Track + position"};
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->resume = (media_resume_t)cycle((int)cfg->resume, MEDIA_RESUME_COUNT, event);
    strcpy(option->value, names[cfg->resume % MEDIA_RESUME_COUNT]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t artwork_bg_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->artwork_background = !cfg->artwork_background;
    strcpy(option->value, cfg->artwork_background ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t dynamic_theme_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->dynamic_theme = !cfg->dynamic_theme;
    strcpy(option->value, cfg->dynamic_theme ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t low_effects_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->low_effects = !cfg->low_effects;
    strcpy(option->value, cfg->low_effects ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t viz_fps_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const int values[] = {15, 20, 30, 45, 60};
    media_settings_t *cfg = media_settings();
    int index = 2;
    for (size_t i = 0; i < RG_COUNT(values); ++i)
    {
        if (values[i] == cfg->visualizer_fps)
            index = (int)i;
    }
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->visualizer_fps = values[cycle(index, (int)RG_COUNT(values), event)];
    sprintf(option->value, "%d fps", cfg->visualizer_fps);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t remember_queue_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->remember_queue = !cfg->remember_queue;
    strcpy(option->value, cfg->remember_queue ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t scan_startup_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->scan_on_startup = !cfg->scan_on_startup;
    strcpy(option->value, cfg->scan_on_startup ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t skip_error_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->skip_on_error = !cfg->skip_on_error;
    strcpy(option->value, cfg->skip_on_error ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t debug_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    media_settings_t *cfg = media_settings();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        cfg->show_debug = !cfg->show_debug;
    strcpy(option->value, cfg->show_debug ? "On" : "Off");
    return RG_DIALOG_VOID;
}

/* -------------------------------------------------------------------------------------- */
/* Sleep timer                                                                              */
/* -------------------------------------------------------------------------------------- */

static void sleep_timer_menu(void)
{
    char values[8][16];
    static const int minutes[] = {0, 15, 30, 45, 60, 90, -1, -2};
    static const char *labels[] = {"Off",     "15 minutes",    "30 minutes",   "45 minutes",
                                   "60 minutes", "90 minutes", "End of track", "End of album"};

    rg_gui_option_t options[RG_COUNT(minutes) + 1];
    int current = media_player_get_sleep_timer();
    int selected = 0;

    for (size_t i = 0; i < RG_COUNT(minutes); ++i)
    {
        values[i][0] = 0;
        if (minutes[i] == current)
        {
            strcpy(values[i], "*");
            selected = (int)i;
        }
        options[i] = (rg_gui_option_t){(intptr_t)i, labels[i], values[i], RG_DIALOG_FLAG_NORMAL, NULL};
    }
    options[RG_COUNT(minutes)] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t choice = rg_gui_dialog("Sleep Timer", options, selected);
    if (choice >= 0 && choice < (intptr_t)RG_COUNT(minutes))
    {
        media_player_set_sleep_timer(minutes[choice]);
        media_settings_save();
    }
}

/* -------------------------------------------------------------------------------------- */
/* Equalizer                                                                                */
/* -------------------------------------------------------------------------------------- */

static void equalizer_preset_menu(void)
{
    rg_gui_option_t options[MEDIA_EQ_PRESET_COUNT + 1];
    char values[MEDIA_EQ_PRESET_COUNT][4];

    for (int i = 0; i < MEDIA_EQ_PRESET_COUNT; ++i)
    {
        values[i][0] = 0;
        if (i == (int)media_eq_get_preset())
            strcpy(values[i], "*");
        options[i] = (rg_gui_option_t){(intptr_t)i, media_eq_preset_name((media_eq_preset_t)i),
                                       values[i], RG_DIALOG_FLAG_NORMAL, NULL};
    }
    options[MEDIA_EQ_PRESET_COUNT] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t choice = rg_gui_dialog("EQ Preset", options, (int)media_eq_get_preset());
    if (choice >= 0 && choice < MEDIA_EQ_PRESET_COUNT)
    {
        media_settings_t *cfg = media_settings();
        media_eq_set_preset((media_eq_preset_t)choice);
        cfg->eq_preset = (media_eq_preset_t)choice;
        for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
            cfg->eq_gains[i] = media_eq_get_gain(i);
    }
}

/**
 * Graphical equaliser. Owns its own loop because it is a direct-manipulation screen rather
 * than a list; music keeps playing throughout.
 */
void media_ui_equalizer_screen(void)
{
    media_settings_t *cfg = media_settings();
    media_layout_t *l = &mui.layout;
    int band = 0;
    bool running = true;
    uint32_t previous = 0;
    int64_t next_repeat = 0;

    while (running)
    {
        int64_t now = rg_system_timer();
        uint32_t keys = rg_input_read_gamepad();
        uint32_t pressed = 0;

        if (keys)
        {
            if (keys != previous)
            {
                pressed = keys;
                next_repeat = now + 350000;
            }
            else if (now >= next_repeat)
            {
                pressed = keys;
                next_repeat = now + 120000;
            }
        }
        previous = keys;

        if (pressed & RG_KEY_LEFT)
            band = (band + MEDIA_EQ_BANDS - 1) % MEDIA_EQ_BANDS;
        if (pressed & RG_KEY_RIGHT)
            band = (band + 1) % MEDIA_EQ_BANDS;
        if (pressed & RG_KEY_UP)
            media_eq_set_gain(band, media_eq_get_gain(band) + 1);
        if (pressed & RG_KEY_DOWN)
            media_eq_set_gain(band, media_eq_get_gain(band) - 1);
        if (pressed & RG_KEY_A)
        {
            cfg->eq_enabled = !cfg->eq_enabled;
            media_eq_set_enabled(cfg->eq_enabled);
        }
        if (pressed & RG_KEY_START)
            equalizer_preset_menu();
        if (pressed & RG_KEY_SELECT)
        {
            media_eq_reset();
            cfg->eq_preset = MEDIA_EQ_PRESET_FLAT;
        }
        if (pressed & (RG_KEY_B | RG_KEY_MENU))
            running = false;

        /* --- Render ------------------------------------------------------------------ */
        media_artwork_lock();
        rg_gui_set_surface(mui.surface);
        media_ui_clear();

        char right[24];
        snprintf(right, sizeof(right), "%s / %s", cfg->eq_enabled ? "On" : "Off",
                 media_eq_preset_name(media_eq_get_preset()));
        media_ui_draw_header("Equalizer", right);

        int graph_top = l->content_top + l->pad * 2;
        int graph_h = l->content_h - l->pad * 4 - l->line_h;
        int graph_left = l->pad * 4;
        int graph_w = l->width - graph_left - l->pad * 3;
        int slot = graph_w / MEDIA_EQ_BANDS;
        int mid = graph_top + graph_h / 2;

        // Scale ticks at +12 / 0 / -12 dB
        rg_gui_draw_rect(graph_left, mid, graph_w, 1, 0, 0, mui.theme.divider);
        rg_gui_draw_text(0, graph_top - l->line_h / 2, graph_left - 2, "+12", mui.theme.divider,
                         C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
        rg_gui_draw_text(0, mid - l->line_h / 2, graph_left - 2, "0", mui.theme.divider,
                         C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
        rg_gui_draw_text(0, graph_top + graph_h - l->line_h / 2, graph_left - 2, "-12",
                         mui.theme.divider, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);

        for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
        {
            int cx = graph_left + slot * i + slot / 2;
            int gain = media_eq_get_gain(i);
            int y = mid - (gain * (graph_h / 2)) / MEDIA_EQ_GAIN_MAX;
            bool selected = i == band;

            rg_gui_draw_rect(cx - 1, graph_top, 2, graph_h, 0, 0,
                             selected ? mui.theme.accent_dim : mui.theme.divider);

            // Fill between zero and the handle so the shape of the curve is readable
            int fill_top = RG_MIN(y, mid);
            int fill_h = abs(y - mid);
            if (fill_h)
                rg_gui_draw_rect(cx - 2, fill_top, 5, fill_h, 0, 0,
                                 cfg->eq_enabled ? mui.theme.accent : mui.theme.divider);

            int knob = selected ? 9 : 7;
            rg_gui_draw_rect(cx - knob / 2, y - knob / 2, knob, knob, 0, 0,
                             selected ? mui.theme.highlight : mui.theme.text_dim);

            char label[8];
            snprintf(label, sizeof(label), "%s", media_eq_band_label(i));
            rg_gui_draw_text(cx - slot / 2, graph_top + graph_h + 2, slot, label,
                             selected ? mui.theme.accent : mui.theme.divider, C_TRANSPARENT,
                             RG_TEXT_ALIGN_CENTER);

            if (selected)
            {
                char value[12];
                snprintf(value, sizeof(value), "%+d dB", gain);
                rg_gui_draw_text(cx - slot, y - l->line_h - 6, slot * 2, value, mui.theme.text,
                                 C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
            }
        }

        media_ui_draw_footer("UP/DOWN: Gain   LEFT/RIGHT: Band   A: On/Off   START: Preset   "
                             "SELECT: Reset");

        rg_gui_set_surface(NULL);
        rg_display_submit(mui.surface, 0);
        media_artwork_unlock();

        rg_task_delay(20);
        media_player_tick();
    }

    for (int i = 0; i < MEDIA_EQ_BANDS; ++i)
        cfg->eq_gains[i] = media_eq_get_gain(i);
    cfg->eq_preset = media_eq_get_preset();
    media_settings_save();

    // Swallow the release so the caller does not immediately act on the same press.
    rg_input_wait_for_key(RG_KEY_ALL, false, 500);
}

/* -------------------------------------------------------------------------------------- */
/* Visualizer picker                                                                        */
/* -------------------------------------------------------------------------------------- */

void media_ui_visualizer_menu(void)
{
    rg_gui_option_t options[MEDIA_VIZ_COUNT + 1];
    char values[MEDIA_VIZ_COUNT][4];
    media_settings_t *cfg = media_settings();

    for (int i = 0; i < MEDIA_VIZ_COUNT; ++i)
    {
        values[i][0] = 0;
        if (i == (int)cfg->visualizer)
            strcpy(values[i], "*");
        options[i] = (rg_gui_option_t){
            (intptr_t)i, media_viz_name((media_viz_t)i), values[i],
            media_viz_available((media_viz_t)i) ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_DISABLED,
            NULL};
    }
    options[MEDIA_VIZ_COUNT] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t choice = rg_gui_dialog("Visualizer", options, (int)cfg->visualizer);
    if (choice >= 0 && choice < MEDIA_VIZ_COUNT && media_viz_available((media_viz_t)choice))
    {
        cfg->visualizer = (media_viz_t)choice;
        media_settings_save();
        mui.page = MEDIA_PAGE_VISUALIZER;
    }
}

/* -------------------------------------------------------------------------------------- */
/* Settings                                                                                 */
/* -------------------------------------------------------------------------------------- */

void media_ui_settings_menu(void)
{
    char value_buffers[16][32];
    for (size_t i = 0; i < RG_COUNT(value_buffers); ++i)
        value_buffers[i][0] = 0;

    rg_gui_option_t options[] = {
        {0, "Background playback", value_buffers[0], RG_DIALOG_FLAG_NORMAL, &background_cb},
        {0, "Resume playback",     value_buffers[1], RG_DIALOG_FLAG_NORMAL, &resume_cb},
        {0, "Remember queue",      value_buffers[2], RG_DIALOG_FLAG_NORMAL, &remember_queue_cb},
        {0, "Scan on startup",     value_buffers[3], RG_DIALOG_FLAG_NORMAL, &scan_startup_cb},
        RG_DIALOG_SEPARATOR,
        {0, "Normalization",       value_buffers[4], RG_DIALOG_FLAG_NORMAL, &normalize_cb},
        {0, "Gapless playback",    value_buffers[5], RG_DIALOG_FLAG_NORMAL, &gapless_cb},
        {0, "Crossfade",           value_buffers[6], RG_DIALOG_FLAG_NORMAL, &crossfade_cb},
        {0, "Skip failed tracks",  value_buffers[7], RG_DIALOG_FLAG_NORMAL, &skip_error_cb},
        RG_DIALOG_SEPARATOR,
        {0, "Album art background", value_buffers[8], RG_DIALOG_FLAG_NORMAL, &artwork_bg_cb},
        {0, "Dynamic theme",       value_buffers[9], RG_DIALOG_FLAG_NORMAL, &dynamic_theme_cb},
        {0, "Low effects mode",    value_buffers[10], RG_DIALOG_FLAG_NORMAL, &low_effects_cb},
        {0, "Visualizer FPS",      value_buffers[11], RG_DIALOG_FLAG_NORMAL, &viz_fps_cb},
        RG_DIALOG_SEPARATOR,
        {0, "Lyrics",              value_buffers[12], RG_DIALOG_FLAG_NORMAL, &lyrics_toggle_cb},
        {0, "Debug overlay",       value_buffers[13], RG_DIALOG_FLAG_NORMAL, &debug_cb},
        RG_DIALOG_END,
    };

    rg_gui_dialog("Media Settings", options, 0);
    media_settings_save();
}

/* -------------------------------------------------------------------------------------- */
/* Context menu                                                                             */
/* -------------------------------------------------------------------------------------- */

/** Ask for a playlist and append `path` to it. */
static void add_to_playlist(const char *path)
{
    media_playlist_info_t *found = calloc(MEDIA_UI_MAX_PLAYLISTS, sizeof(media_playlist_info_t));
    if (!found)
        return;

    int count = media_playlist_list(media_library_root(), found, MEDIA_UI_MAX_PLAYLISTS);
    rg_gui_option_t options[MEDIA_UI_MAX_PLAYLISTS + 2];
    int n = 0;

    options[n++] = (rg_gui_option_t){-1, "New playlist...", NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    for (int i = 0; i < count && n < MEDIA_UI_MAX_PLAYLISTS + 1; ++i)
        options[n++] = (rg_gui_option_t){(intptr_t)i, found[i].name, NULL, RG_DIALOG_FLAG_NORMAL,
                                         NULL};
    options[n] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t choice = rg_gui_dialog("Add to Playlist", options, 0);

    if (choice == -1)
    {
        char *name = rg_gui_input_str("New playlist", "Name", "Favorites");
        if (name && name[0])
        {
            char folder[MEDIA_MAX_PATH + 32];
            char target[MEDIA_MAX_PATH + 64];
            snprintf(folder, sizeof(folder), "%s/Playlists", media_library_root());
            rg_storage_mkdir(folder);
            snprintf(target, sizeof(target), "%s/%s.m3u8", folder, name);
            if (media_playlist_append(target, path))
                rg_gui_alert("Playlist", "Track added.");
            else
                rg_gui_alert("Playlist", "Could not write the playlist.");
        }
        free(name);
    }
    else if (choice >= 0 && choice < count)
    {
        if (media_playlist_append(found[choice].path, path))
            rg_gui_alert("Playlist", "Track added.");
        else
            rg_gui_alert("Playlist", "Could not write the playlist.");
    }

    free(found);
}

/** Queue every audio file in `folder` (non-recursive) and optionally start playing. */
typedef struct
{
    int added;
} folder_queue_t;

static int folder_queue_cb(const rg_scandir_t *file, void *arg)
{
    folder_queue_t *state = arg;

    if (!file->is_file || media_path_is_hidden(file->basename))
        return RG_SCANDIR_CONTINUE;
    if (!media_path_is_audio(file->basename))
        return RG_SCANDIR_CONTINUE;

    if (media_queue_add(file->path, media_library_find_by_path(file->path)))
        state->added++;

    return RG_SCANDIR_CONTINUE;
}

void media_ui_context_menu(const media_list_item_t *item, const char *path, uint32_t track_id)
{
    if (!item)
        return;

    bool is_track = item->kind == 2 && path;
    bool is_folder = item->kind == 1 && path;

    if (!is_track && !is_folder)
        return;

    bool favorite = track_id && media_library_is_favorite(track_id);

    rg_gui_option_t track_options[] = {
        {0, "Play",             NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {1, "Play next",        NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, "Add to queue",     NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3, "Add to playlist",  NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {4, favorite ? "Remove favorite" : "Add favorite", NULL,
            track_id ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_DISABLED, NULL},
        {5, "Track information", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };

    rg_gui_option_t folder_options[] = {
        {10, "Play folder",         NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {11, "Shuffle folder",      NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {12, "Add folder to queue", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };

    intptr_t choice = rg_gui_dialog(item->text, is_track ? track_options : folder_options, 0);

    switch (choice)
    {
    case 0:
        media_player_play_path(path, track_id);
        mui.page = MEDIA_PAGE_NOW_PLAYING;
        break;

    case 1:
        media_queue_add_next(path, track_id);
        break;

    case 2:
        media_queue_add(path, track_id);
        break;

    case 3:
        add_to_playlist(path);
        break;

    case 4:
        if (track_id)
            media_library_set_favorite(track_id, !favorite);
        media_ui_library_refresh();
        break;

    case 5:
    {
        // Load the track's own metadata so Track Info works before it has ever been played.
        media_track_t track;
        if (track_id && media_library_get_track(track_id, &track))
        {
            char body[256];
            char duration[16];
            media_format_time(duration, sizeof(duration), track.duration_ms);
            snprintf(body, sizeof(body), "%s\n%s\n%s\n\n%s  %s  %u kbps",
                     track.title, track.artist[0] ? track.artist : "Unknown Artist",
                     track.album[0] ? track.album : "Unknown Album",
                     media_codec_name((media_codec_t)track.codec), duration,
                     (unsigned)(track.bitrate / 1000));
            rg_gui_alert("Track Information", body);
        }
        else
        {
            rg_gui_alert("Track Information", rg_basename(path));
        }
        break;
    }

    case 10:
    case 11:
    case 12:
    {
        folder_queue_t state = {0};
        if (choice != 12)
            media_queue_clear();
        rg_storage_scandir(path, folder_queue_cb, &state, RG_SCANDIR_FILES | RG_SCANDIR_SORT);

        if (state.added == 0)
        {
            rg_gui_alert("Folder", "No playable tracks here.");
            break;
        }

        if (choice == 11)
        {
            media_player_set_shuffle(true);
            media_queue_reshuffle();
        }
        if (choice != 12)
        {
            media_player_play_index(media_queue_index() >= 0 ? media_queue_index() : 0);
            mui.page = MEDIA_PAGE_NOW_PLAYING;
        }
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------------------------------------- */
/* Player menu                                                                              */
/* -------------------------------------------------------------------------------------- */

void media_ui_player_menu(void)
{
    char values[6][32];
    for (size_t i = 0; i < RG_COUNT(values); ++i)
        values[i][0] = 0;

    bool playing = mui.snapshot.state != MEDIA_STATE_STOPPED;
    int flag_playing = playing ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_DISABLED;

    rg_gui_option_t options[] = {
        {1,  "Now Playing",      NULL, flag_playing, NULL},
        {2,  "Browse Media",     NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3,  "Queue",            NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_SEPARATOR,
        {0,  "Shuffle",          values[0], RG_DIALOG_FLAG_NORMAL, &shuffle_cb},
        {0,  "Repeat",           values[1], RG_DIALOG_FLAG_NORMAL, &repeat_cb},
        {4,  "Favorite",         NULL, mui.snapshot.track_id ? RG_DIALOG_FLAG_NORMAL
                                                             : RG_DIALOG_FLAG_DISABLED, NULL},
        RG_DIALOG_SEPARATOR,
        {5,  "Equalizer",        NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {6,  "Visualizer",       NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {0,  "Lyrics",           values[2], RG_DIALOG_FLAG_NORMAL, &lyrics_toggle_cb},
        {0,  "Lyrics offset",    values[3], RG_DIALOG_FLAG_NORMAL, &lyrics_offset_cb},
        RG_DIALOG_SEPARATOR,
        {7,  "Sleep timer",      NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {8,  "Track information", NULL, flag_playing, NULL},
        {9,  "Rescan library",   NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {10, "Media settings",   NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_SEPARATOR,
        {11, "Exit media player", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };

    intptr_t choice = rg_gui_dialog("Media Player", options, 0);

    switch (choice)
    {
    case 1:
        mui.page = MEDIA_PAGE_NOW_PLAYING;
        mui.in_library = false;
        break;

    case 2:
        mui.page = MEDIA_PAGE_LIBRARY;
        mui.in_library = true;
        media_ui_library_refresh();
        break;

    case 3:
        mui.page = MEDIA_PAGE_QUEUE;
        media_ui_queue_refresh();
        break;

    case 4:
        media_player_toggle_favorite();
        break;

    case 5:
        media_ui_equalizer_screen();
        break;

    case 6:
        media_ui_visualizer_menu();
        break;

    case 7:
        sleep_timer_menu();
        break;

    case 8:
        mui.page = MEDIA_PAGE_INFO;
        break;

    case 9:
    {
        rg_gui_option_t rescan[] = {
            {0, "Quick rescan", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {1, "Full rebuild", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            RG_DIALOG_END,
        };
        intptr_t mode = rg_gui_dialog("Rescan Library", rescan, 0);
        if (mode == 0 || mode == 1)
        {
            if (!media_library_scan_start(mode == 1))
                rg_gui_alert("Rescan", "Could not start the scan.\nIs the media folder present?");
        }
        break;
    }

    case 10:
        media_ui_settings_menu();
        break;

    case 11:
        mui.running = false;
        break;

    default:
        break;
    }

    media_settings_save();
}
