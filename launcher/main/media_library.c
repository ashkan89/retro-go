#include "media_library.h"

#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gui.h"
#include "media_metadata.h"
#include "media_player.h"

typedef enum { ENTRY_DIRECTORY, ENTRY_MP3 } entry_type_t;
typedef struct {
    char *name;
    entry_type_t type;
    size_t size;
    media_metadata_t metadata;
} media_entry_t;
typedef struct { char path[RG_PATH_MAX + 1]; media_metadata_t metadata; } playlist_track_t;

static media_entry_t *entries;
static size_t entry_count, entry_capacity;
static char current_path[RG_PATH_MAX + 1];
static tab_t *media_tab;
static int current_track = -1;
static playlist_track_t *playlist;
static size_t playlist_count;
static rg_image_t *player_cover;
static media_lyrics_t player_lyrics;

static bool is_mp3(const char *name) { return rg_extension_match(name, "mp3") == 0; }

static int entry_compare(const void *a, const void *b)
{
    const media_entry_t *aa = a, *bb = b;
    if (aa->type != bb->type) return aa->type - bb->type;
    return strcasecmp(aa->name, bb->name);
}

static int scan_cb(const rg_scandir_t *item, void *arg)
{
    if (!item->is_dir && !(item->is_file && is_mp3(item->basename))) return RG_SCANDIR_CONTINUE;
    if (entry_count == entry_capacity) {
        size_t capacity = entry_capacity ? entry_capacity * 2 : 32;
        media_entry_t *resized = realloc(entries, capacity * sizeof(*entries));
        if (!resized) return RG_SCANDIR_STOP;
        entries = resized; entry_capacity = capacity;
    }
    media_entry_t *entry = &entries[entry_count++]; memset(entry, 0, sizeof(*entry));
    entry->name = strdup(item->basename); entry->size = item->size;
    entry->type = item->is_dir ? ENTRY_DIRECTORY : ENTRY_MP3;
    if (entry->type == ENTRY_MP3) media_metadata_read(item->path, &entry->metadata, true);
    return RG_SCANDIR_CONTINUE;
}

static void clear_entries(void)
{
    for (size_t i = 0; i < entry_count; i++) free(entries[i].name);
    entry_count = 0;
}

static void scan_directory(tab_t *tab, const char *path, const char *selection)
{
    clear_entries(); snprintf(current_path, sizeof(current_path), "%s", path);
    tab->navpath = current_path;
    rg_storage_scandir(path, scan_cb, NULL, RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_STAT);
    qsort(entries, entry_count, sizeof(*entries), entry_compare);
    gui_resize_list(tab, entry_count); tab->listbox.cursor = 0;
    for (size_t i = 0; i < entry_count; i++) {
        listbox_item_t *item = &tab->listbox.items[i]; media_entry_t *entry = &entries[i];
        item->arg = entry; item->group = entry->type == ENTRY_DIRECTORY ? 1 : 2;
        if (entry->type == ENTRY_DIRECTORY) snprintf(item->text, sizeof(item->text), "[%.89s]", entry->name);
        else if (entry->metadata.artist[0]) snprintf(item->text, sizeof(item->text), "%.45s - %.40s", entry->metadata.title, entry->metadata.artist);
        else snprintf(item->text, sizeof(item->text), "%.91s", entry->metadata.title);
        if (selection && !strcmp(entry->name, selection)) tab->listbox.cursor = i;
    }
    gui_scroll_list(tab, SCROLL_SET, tab->listbox.cursor);
}

static void path_for_entry(const media_entry_t *entry, char *out, size_t size)
{
    size_t a = strlen(current_path), b = strlen(entry->name);
    if (a + 1 + b >= size) { out[0] = 0; return; }
    memcpy(out, current_path, a); out[a] = '/'; memcpy(out + a + 1, entry->name, b + 1);
}

static int track_index(const media_entry_t *entry)
{
    return entry && entry >= entries && entry < entries + entry_count ? entry - entries : -1;
}

static int adjacent_track(int from, int direction)
{
    if (!playlist_count) return -1;
    return (from + direction + playlist_count) % playlist_count;
}

static bool play_playlist_track(int index)
{
    if (index < 0 || index >= (int)playlist_count) return false;
    const char *path = playlist[index].path;
    if (!media_player_play(path, &playlist[index].metadata, 0)) return false;
    current_track = index;
    rg_surface_free(player_cover);
    player_cover = media_metadata_load_cover(path, &playlist[index].metadata, gui.width, gui.height);
    media_lyrics_load(path, &player_lyrics);
    return true;
}

static bool play_track(int entry_index)
{
    if (entry_index < 0 || entry_index >= (int)entry_count || entries[entry_index].type != ENTRY_MP3) return false;
    size_t count = 0;
    for (size_t i = 0; i < entry_count; i++) if (entries[i].type == ENTRY_MP3) count++;
    playlist_track_t *next = calloc(count, sizeof(*next));
    if (!next) return false;
    int selected = -1; size_t out = 0;
    for (size_t i = 0; i < entry_count; i++) if (entries[i].type == ENTRY_MP3) {
        path_for_entry(&entries[i], next[out].path, sizeof(next[out].path));
        next[out].metadata = entries[i].metadata;
        if ((int)i == entry_index) selected = out;
        out++;
    }
    free(playlist); playlist = next; playlist_count = count;
    return play_playlist_track(selected);
}

static void shade_surface(rg_surface_t *surface, int divisor)
{
    for (int y = 0; y < surface->height; y++) {
        uint16_t *line = (uint16_t *)((uint8_t *)surface->data + y * surface->stride);
        for (int x = 0; x < surface->width; x++) {
            uint16_t p = line[x]; line[x] = (((p >> 11) / divisor) << 11) | ((((p >> 5) & 63) / divisor) << 5) | ((p & 31) / divisor);
        }
    }
}

static void draw_backdrop(void)
{
    if (player_cover) {
        rg_surface_copy(player_cover, NULL, gui.surface, NULL, true); shade_surface(gui.surface, 3);
    } else {
        rg_surface_fill(gui.surface, NULL, C_MIDNIGHT_BLUE);
        for (int y = 0; y < gui.height; y += 8) {
            int blue = 8 + y * 18 / RG_MAX(1, gui.height);
            rg_gui_draw_rect(0, y, gui.width, 8, 0, 0, C_RGB(2, 8, blue));
        }
    }
}

static void draw_progress(const media_player_snapshot_t *s)
{
    int margin = RG_MAX(10, gui.width / 24), bar_y = gui.height - 38, bar_w = gui.width - margin * 2;
    uint32_t duration = RG_MAX(1, s->duration_ms);
    int buffered = (uint64_t)bar_w * s->buffered_ms / duration;
    int played = (uint64_t)bar_w * s->position_ms / duration;
    rg_gui_draw_rect(margin, bar_y, bar_w, 7, 1, C_SILVER, C_DARK_SLATE_GRAY);
    rg_gui_draw_rect(margin + 1, bar_y + 1, RG_MIN(bar_w - 2, buffered), 5, 0, 0, C_SLATE_GRAY);
    rg_gui_draw_rect(margin + 1, bar_y + 1, RG_MIN(bar_w - 2, played), 5, 0, 0, C_DEEP_SKY_BLUE);
    char elapsed[16], total[16], remaining[20]; media_format_time(s->position_ms, elapsed, sizeof(elapsed));
    media_format_time(s->duration_ms, total, sizeof(total));
    media_format_time(s->duration_ms > s->position_ms ? s->duration_ms - s->position_ms : 0, remaining + 1, sizeof(remaining) - 1); remaining[0] = '-';
    rg_gui_draw_text(margin, bar_y - 15, bar_w / 3, elapsed, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(margin + bar_w / 3, bar_y - 15, bar_w / 3, total, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(margin + 2 * bar_w / 3, bar_y - 15, bar_w / 3, remaining, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
}

static void draw_now_playing(const media_player_snapshot_t *s)
{
    int margin = RG_MAX(10, gui.width / 25), panel_y = gui.height > 280 ? gui.height / 2 : 45;
    rg_gui_draw_rect(margin, panel_y, gui.width - margin * 2, gui.height - panel_y - 48, 1, C_DARK_SLATE_GRAY, C_NAVY);
    rg_gui_draw_text_bidi(margin + 8, panel_y + 7, gui.width - margin * 2 - 16, s->metadata.title, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
    rg_gui_draw_text_bidi(margin + 8, panel_y + 33, gui.width - margin * 2 - 16,
                          s->metadata.artist[0] ? s->metadata.artist : "Unknown artist", C_LIGHT_CYAN, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text_bidi(margin + 8, panel_y + 50, gui.width - margin * 2 - 16, s->metadata.album, C_SILVER, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

static void draw_lyrics(const media_player_snapshot_t *s)
{
    int active = media_lyrics_find(&player_lyrics, s->position_ms);
    if (!player_lyrics.count) {
        const char *text = s->metadata.lyrics[0] ? s->metadata.lyrics : "No synchronized lyrics\nAdd a UTF-8 .lrc file beside this MP3.";
        rg_gui_draw_text_bidi(12, 65, gui.width - 24, text, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER | RG_TEXT_MULTILINE);
        return;
    }
    int line_h = 22, center = gui.height / 2 - 8;
    for (int rel = -3; rel <= 3; rel++) {
        int i = active + rel; if (i < 0 || i >= (int)player_lyrics.count) continue;
        rg_color_t color = rel == 0 ? C_AQUA : (abs(rel) == 1 ? C_WHITE : C_GRAY);
        uint32_t flags = RG_TEXT_ALIGN_CENTER | (rel == 0 ? RG_TEXT_BIGGER : 0);
        rg_gui_draw_text_bidi(10, center + rel * line_h, gui.width - 20, player_lyrics.lines[i].text, color, C_TRANSPARENT, flags);
    }
}

static void draw_details(const media_player_snapshot_t *s)
{
    char line[192], size[32]; snprintf(size, sizeof(size), "%.2f MB", s->metadata.audio_size / 1048576.0f);
    int y = 45, h = 20;
#define DETAIL(label, value) do { snprintf(line, sizeof(line), "%s: %s", label, (value)[0] ? (value) : "-"); rg_gui_draw_text_bidi(14, y, gui.width - 28, line, C_WHITE, C_TRANSPARENT, 0); y += h; } while (0)
    DETAIL("Title", s->metadata.title); DETAIL("Artist", s->metadata.artist); DETAIL("Album", s->metadata.album);
    DETAIL("Genre", s->metadata.genre); DETAIL("Year", s->metadata.year); DETAIL("Track", s->metadata.track);
    snprintf(line, sizeof(line), "%lu kbps  |  %lu Hz  |  %s  |  %s", (unsigned long)(s->metadata.bitrate / 1000),
             (unsigned long)s->metadata.sample_rate, s->metadata.channels == 1 ? "Mono" : "Stereo", size);
    rg_gui_draw_text(14, y, gui.width - 28, line, C_LIGHT_CYAN, C_TRANSPARENT, RG_TEXT_MULTILINE);
#undef DETAIL
}

static void draw_spectrum(const media_player_snapshot_t *s)
{
    int margin = 14, gap = 3, usable = gui.width - margin * 2, bw = (usable - 15 * gap) / 16;
    int base = gui.height - 52, max_h = gui.height - 105;
    for (int i = 0; i < 16; i++) {
        int height = RG_MAX(2, max_h * s->spectrum[i] / 100);
        rg_color_t color = i < 5 ? C_DEEP_SKY_BLUE : i < 11 ? C_MEDIUM_SPRING_GREEN : C_ORANGE;
        rg_gui_draw_rect(margin + i * (bw + gap), base - height, bw, height, 0, 0, color);
        rg_gui_draw_rect(margin + i * (bw + gap), base - max_h, bw, max_h - height, 0, 0, C_DARK_SLATE_GRAY);
    }
    rg_gui_draw_text(0, 46, gui.width, "LIVE SPECTRUM", C_AQUA, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
    rg_gui_draw_text(0, base + 5, gui.width, "60   150   400    1k    2.5k    6k    14k Hz", C_SILVER, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

static void draw_player(int page)
{
    media_player_snapshot_t s; media_player_get_snapshot(&s);
    rg_display_sync(true); rg_gui_set_surface(gui.surface); draw_backdrop();
    static const char *pages[] = {"NOW PLAYING", "SYNCED LYRICS", "TRACK DETAILS", "SPECTRUM"};
    rg_gui_draw_rect(0, 0, gui.width, 32, 0, 0, C_NAVY);
    rg_gui_draw_text(10, 7, gui.width - 20, pages[page], C_AQUA, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    char status[32]; snprintf(status, sizeof(status), "%s  VOL %d%%", s.state == MEDIA_PAUSED ? "PAUSED" : s.state == MEDIA_BUFFERING ? "BUFFERING" : s.state == MEDIA_ERROR ? "ERROR" : "PLAYING", rg_audio_get_volume());
    rg_gui_draw_text(5, 7, gui.width - 10, status, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
    if (page == 0) draw_now_playing(&s); else if (page == 1) draw_lyrics(&s); else if (page == 2) draw_details(&s); else draw_spectrum(&s);
    draw_progress(&s);
    rg_gui_draw_text(0, gui.height - 19, gui.width, "A Pause  Start Stop  Option+Arrows Track/Light", C_SILVER, C_NAVY, RG_TEXT_ALIGN_CENTER);
    if (s.state == MEDIA_ERROR) rg_gui_draw_text(10, gui.height / 2, gui.width - 20, s.error, C_LIGHT_CORAL, C_NAVY, RG_TEXT_ALIGN_CENTER);
    rg_gui_set_surface(NULL); rg_display_submit(gui.surface, 0);
}

static void show_player(void)
{
    int page = 0; uint32_t previous = 0; int64_t next_draw = 0, repeat_at = 0;
    int old_font = rg_gui_get_font(); rg_gui_set_font(RG_GUI_FONT_MEDIA);
    rg_input_wait_for_key(RG_KEY_ALL, false, 500);
    while (true) {
        uint32_t keys = rg_input_read_gamepad(); uint32_t pressed = keys & ~previous;
        if (pressed & RG_KEY_B) break;
        if (pressed & RG_KEY_A) {
            media_player_snapshot_t state; media_player_get_snapshot(&state);
            if (state.state == MEDIA_STOPPED || state.state == MEDIA_ERROR) play_playlist_track(current_track);
            else media_player_toggle_pause();
        }
        if (pressed & RG_KEY_START) media_player_stop();
        if (pressed & RG_KEY_SELECT) page = (page + 1) % 4;
        if (pressed & RG_KEY_L) { int i = adjacent_track(current_track, -1); if (i >= 0) play_playlist_track(i); }
        if (pressed & RG_KEY_R) { int i = adjacent_track(current_track, 1); if (i >= 0) play_playlist_track(i); }
        if (pressed & RG_KEY_UP) {
            if (keys & RG_KEY_OPTION) rg_display_set_backlight(RG_MIN(100, rg_display_get_backlight() + 10));
            else rg_audio_set_volume(RG_MIN(100, rg_audio_get_volume() + 5));
        }
        if (pressed & RG_KEY_DOWN) {
            if (keys & RG_KEY_OPTION) rg_display_set_backlight(RG_MAX(1, rg_display_get_backlight() - 10));
            else rg_audio_set_volume(RG_MAX(0, rg_audio_get_volume() - 5));
        }
        if (pressed & RG_KEY_LEFT) {
            if (keys & RG_KEY_OPTION) { int i = adjacent_track(current_track, -1); if (i >= 0) play_playlist_track(i); }
            else media_player_seek(-10000);
        }
        if (pressed & RG_KEY_RIGHT) {
            if (keys & RG_KEY_OPTION) { int i = adjacent_track(current_track, 1); if (i >= 0) play_playlist_track(i); }
            else media_player_seek(10000);
        }
        if (pressed & RG_KEY_Y) rg_display_set_backlight(RG_MIN(100, rg_display_get_backlight() + 10));
        if (pressed & RG_KEY_X) rg_display_set_backlight(RG_MAX(1, rg_display_get_backlight() - 10));
        if (keys != previous) repeat_at = rg_system_timer() + 450000;
        else if (!(keys & RG_KEY_OPTION) && (keys & (RG_KEY_LEFT | RG_KEY_RIGHT)) && rg_system_timer() >= repeat_at) {
            media_player_seek(keys & RG_KEY_LEFT ? -10000 : 10000); repeat_at = rg_system_timer() + 180000;
        }
        if (media_player_take_finished()) { int i = adjacent_track(current_track, 1); if (i >= 0) play_playlist_track(i); }
        if (rg_system_timer() >= next_draw || pressed) { draw_player(page); next_draw = rg_system_timer() + 50000; }
        previous = keys; rg_task_delay(5);
    }
    rg_gui_set_font(old_font); rg_input_wait_for_key(RG_KEY_ALL, false, 300); gui_redraw();
}

static void update_browser_preview(tab_t *tab, media_entry_t *entry)
{
    gui_set_preview(tab, NULL);
    if (!entry) return;
    char path[RG_PATH_MAX + 1], status[24] = ""; path_for_entry(entry, path, sizeof(path));
    if (entry->type == ENTRY_MP3) {
        gui_set_preview(tab, media_metadata_load_cover(path, &entry->metadata, gui.width / 2, gui.height * 2 / 3));
        char duration[12]; media_format_time(entry->metadata.duration_ms, duration, sizeof(duration));
        snprintf(status, sizeof(status), "%s %luk", duration, (unsigned long)(entry->metadata.bitrate / 1000));
    } else {
        media_metadata_t empty = {0};
        char probe[RG_PATH_MAX + 1];
        size_t length = strlen(path);
        if (length + 2 < sizeof(probe)) { memcpy(probe, path, length); memcpy(probe + length, "/_", 3);
            gui_set_preview(tab, media_metadata_load_cover(probe, &empty, gui.width / 2, gui.height * 2 / 3)); }
        snprintf(status, sizeof(status), "Album folder");
    }
    gui_set_status(tab, NULL, status);
}

static void event_handler(gui_event_t event, tab_t *tab)
{
    listbox_item_t *item = gui_get_selected_item(tab);
    media_entry_t *entry = item ? item->arg : NULL;
    if (event == TAB_INIT) scan_directory(tab, current_path, NULL);
    else if (event == TAB_ENTER || event == TAB_SCROLL) update_browser_preview(tab, entry);
    else if (event == TAB_LEAVE) gui_set_preview(tab, NULL);
    else if (event == TAB_ACTION && entry) {
        char path[RG_PATH_MAX + 1]; path_for_entry(entry, path, sizeof(path));
        if (entry->type == ENTRY_DIRECTORY) scan_directory(tab, path, NULL);
        else if (play_track(track_index(entry))) show_player();
    } else if (event == TAB_BACK) {
        if (!strcmp(current_path, RG_BASE_PATH_MEDIA)) tab->navpath = NULL;
        else { char selected[RG_PATH_MAX + 1]; snprintf(selected, sizeof(selected), "%s", rg_basename(current_path));
            char parent[RG_PATH_MAX + 1]; snprintf(parent, sizeof(parent), "%s", current_path); char *slash = strrchr(parent, '/'); if (slash) *slash = 0;
            scan_directory(tab, parent, selected); }
    } else if (event == TAB_IDLE && media_player_take_finished()) {
        int i = adjacent_track(current_track, 1); if (i >= 0) play_playlist_track(i);
    }
}

void media_library_init(void)
{
    snprintf(current_path, sizeof(current_path), "%s", RG_BASE_PATH_MEDIA);
    if (!rg_storage_exists(current_path)) rg_storage_mkdir(current_path);
    if (!media_player_init()) RG_LOGE("Media player tasks could not start");
    media_tab = gui_add_tab("music", "Music Player", NULL, event_handler);
}
