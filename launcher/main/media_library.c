#include "media_library.h"

#include <rg_system.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gui.h"
#include "media_dsp.h"
#include "media_metadata.h"
#include "media_player.h"

#ifdef ESP_PLATFORM
#include <esp_random.h>
#endif

#if defined(ESP_PLATFORM) && defined(RG_GPIO_HEADPHONE_DETECT)
#include <driver/gpio.h>
#ifndef RG_HEADPHONE_DETECT_LEVEL
#define RG_HEADPHONE_DETECT_LEVEL 0
#endif
#ifndef RG_HEADPHONE_DETECT_PULLUP
#define RG_HEADPHONE_DETECT_PULLUP 1
#endif
#endif

#define SETTING_MEDIA_PATH "MediaPath"
#define SETTING_MEDIA_POSITION "MediaPosition"
#define SETTING_MEDIA_PLAY_MODE "MediaPlayMode"
#define SETTING_MEDIA_SLEEP "MediaSleepMinutes"
#define SETTING_MEDIA_EQ_ON "MediaEqOn"
#define SETTING_MEDIA_EQ_PRESET "MediaEqPreset"
#define SETTING_MEDIA_EQ_GAIN "MediaEqGain%d"
#define SETTING_MEDIA_SPECTRUM "MediaSpectrumStyle"

/* Committing settings writes a JSON file to the SD card, which competes with
   the streaming reads. Keep the in-memory value fresh but flush rarely; every
   state change (pause, stop, track change, leaving the player) flushes anyway,
   so the only thing this costs is resume accuracy after a hard power loss. */
#define RESUME_TOUCH_INTERVAL_US 5000000
#define RESUME_COMMIT_INTERVAL_US 120000000

#define SPECTRUM_BANDS 24
#define MEDIA_PI 3.14159265358979f
/* rg_gui's C_RGB takes 8-bit red and green but a raw 5-bit blue, which is easy
   to get wrong. This one takes 0-255 for all three channels. */
#define MEDIA_RGB(r, g, b) ((rg_color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* Tag parsing hits the SD card, so the queue page fills in at most one row per
   frame rather than stalling playback while a long folder is indexed. */
#define QUEUE_SUMMARIES_PER_FRAME 1

typedef enum { ENTRY_DIRECTORY, ENTRY_PLAYLIST, ENTRY_MEDIA } entry_type_t;
typedef enum { PLAY_REPEAT_ALL, PLAY_REPEAT_ONE, PLAY_SHUFFLE, PLAY_MODE_COUNT } play_mode_t;

typedef enum {
    SPECTRUM_STYLE_BARS,
    SPECTRUM_STYLE_BLOCKS,
    SPECTRUM_STYLE_MIRROR,
    SPECTRUM_STYLE_WAVE,
    SPECTRUM_STYLE_COUNT,
} spectrum_style_t;

typedef enum {
    PAGE_NOW_PLAYING,
    PAGE_SPECTRUM,
    PAGE_VU,
    PAGE_WAVEFORM,
    PAGE_LYRICS,
    PAGE_EQUALIZER,
    PAGE_QUEUE,
    PAGE_DETAILS,
    PAGE_COUNT,
} page_t;

typedef struct {
    char *name;
    entry_type_t type;
    size_t size;
    bool metadata_loaded;
    media_metadata_t metadata;
} media_entry_t;

/* The queue only keeps what the queue page needs; full metadata is re-read
   when a track actually starts, which keeps a 1000 track folder affordable. */
typedef struct {
    char path[RG_PATH_MAX + 1];
    char title[64];
    uint32_t duration_ms;
    bool summary_loaded;
} playlist_track_t;

static media_entry_t *entries;
static size_t entry_count, entry_capacity;
static char current_path[RG_PATH_MAX + 1];
static tab_t *media_tab;

/* Loading a preview means reading tags and decoding a JPEG, several kilobytes
   of stack. gui_scroll_list() fires TAB_SCROLL from inside scan_directory(),
   which would run all of that nested three frames deeper than normal -- deep
   enough to overflow the UI task's stack when opening a folder. The scan just
   records that a preview is wanted and the main loop picks it up at the top
   level on its next pass. */
static bool scanning_directory;
static bool preview_pending;

static int current_track = -1;
static playlist_track_t *playlist;
static size_t playlist_count;
static int *shuffle_order;
static size_t shuffle_size, shuffle_pos;

/* Tags of the track that is loaded, kept here rather than in the player
   snapshot so that reading playback state stays cheap on the stack. */
static media_metadata_t current_meta;
/* Scratch for the throwaway reads (queue summaries, folder cover probes).
   media_metadata_t is 1.3 KB, which is far too much to put on the UI stack in
   the middle of a browse-and-decode-a-cover call chain. Single-threaded: only
   the launcher UI thread parses tags. */
static media_metadata_t scratch_meta;

static rg_image_t *player_cover;
static media_lyrics_t player_lyrics;
static bool artwork_pending;
static bool has_cover;
static char artwork_path[RG_PATH_MAX + 1];

static play_mode_t play_mode;
static char *resume_path;
static uint32_t resume_position;
static uint32_t last_saved_position;
static int64_t next_resume_touch;
static int64_t next_resume_commit;
static int64_t sleep_deadline;
static uint16_t sleep_minutes;
static bool media_ready;

static spectrum_style_t spectrum_style;

#if defined(ESP_PLATFORM) && defined(RG_GPIO_HEADPHONE_DETECT)
static bool headphone_present;
static bool headphone_raw;
static bool paused_for_headphones;
static int64_t headphone_changed_at;
#endif

static bool is_media(const char *name) { return media_format_from_path(name) != MEDIA_FORMAT_UNKNOWN; }
static bool is_playlist(const char *name) { return rg_extension_match(name, "m3u m3u8"); }

/*******************************************************************************
 * Library browsing
 ******************************************************************************/

static int entry_compare(const void *a, const void *b)
{
    const media_entry_t *aa = a, *bb = b;
    if (aa->type != bb->type) return aa->type - bb->type;
    return strcasecmp(aa->name, bb->name);
}

static int scan_cb(const rg_scandir_t *item, void *arg)
{
    if (!item->is_dir && !(item->is_file && (is_media(item->basename) || is_playlist(item->basename))))
        return RG_SCANDIR_CONTINUE;
    if (entry_count == entry_capacity) {
        size_t capacity = entry_capacity ? entry_capacity * 2 : 32;
        media_entry_t *resized = realloc(entries, capacity * sizeof(*entries));
        if (!resized) return RG_SCANDIR_STOP;
        entries = resized; entry_capacity = capacity;
    }
    media_entry_t *entry = &entries[entry_count++];
    memset(entry, 0, sizeof(*entry));
    entry->name = strdup(item->basename);
    entry->size = item->size;
    entry->type = item->is_dir ? ENTRY_DIRECTORY : is_playlist(item->basename) ? ENTRY_PLAYLIST : ENTRY_MEDIA;
    return RG_SCANDIR_CONTINUE;
}

static void clear_entries(void)
{
    for (size_t i = 0; i < entry_count; i++) free(entries[i].name);
    entry_count = 0;
}

static void format_entry_text(listbox_item_t *item, const media_entry_t *entry)
{
    if (entry->type == ENTRY_DIRECTORY)
        snprintf(item->text, sizeof(item->text), "[%.89s]", entry->name);
    else if (entry->type == ENTRY_PLAYLIST)
        snprintf(item->text, sizeof(item->text), "{Playlist} %.80s", entry->name);
    else if (entry->metadata_loaded && entry->metadata.artist[0])
        snprintf(item->text, sizeof(item->text), "%.45s - %.40s", entry->metadata.title, entry->metadata.artist);
    else
        snprintf(item->text, sizeof(item->text), "%.91s", entry->metadata_loaded ? entry->metadata.title : entry->name);
}

static void scan_directory(tab_t *tab, const char *path, const char *selection)
{
    clear_entries();
    snprintf(current_path, sizeof(current_path), "%s", path);
    tab->navpath = current_path;
    rg_storage_scandir(path, scan_cb, NULL, RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_STAT);
    qsort(entries, entry_count, sizeof(*entries), entry_compare);
    gui_resize_list(tab, entry_count);
    /* The resize can fail to allocate, in which case it keeps the old, shorter
       array. Writing entry_count items into it would run off the end. */
    if ((int)entry_count > tab->listbox.length)
        entry_count = tab->listbox.length;
    tab->listbox.cursor = 0;
    for (size_t i = 0; i < entry_count; i++) {
        listbox_item_t *item = &tab->listbox.items[i];
        media_entry_t *entry = &entries[i];
        item->arg = entry;
        item->group = entry->type == ENTRY_DIRECTORY ? 1 : entry->type == ENTRY_PLAYLIST ? 2 : 3;
        format_entry_text(item, entry);
        if (selection && !strcmp(entry->name, selection)) tab->listbox.cursor = i;
    }
    gui_set_status(tab, "", "");
    scanning_directory = true;
    gui_scroll_list(tab, SCROLL_SET, tab->listbox.cursor);
    scanning_directory = false;
    preview_pending = true;
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

/*******************************************************************************
 * Queue and play order
 ******************************************************************************/

static void rebuild_shuffle(int start_from)
{
    free(shuffle_order);
    shuffle_order = playlist_count ? calloc(playlist_count, sizeof(*shuffle_order)) : NULL;
    shuffle_size = shuffle_order ? playlist_count : 0;
    shuffle_pos = 0;
    if (!shuffle_size)
        return;
    for (size_t i = 0; i < shuffle_size; i++)
        shuffle_order[i] = i;
    /* Fisher-Yates. A bag guarantees every track plays once per cycle, unlike
       the previous "pick a random index" approach which repeated constantly. */
    for (size_t i = shuffle_size - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        int swap = shuffle_order[i];
        shuffle_order[i] = shuffle_order[j];
        shuffle_order[j] = swap;
    }
    /* Make the track the user picked the head of the bag. */
    for (size_t i = 0; i < shuffle_size; i++) {
        if (shuffle_order[i] == start_from) {
            shuffle_order[i] = shuffle_order[0];
            shuffle_order[0] = start_from;
            break;
        }
    }
}

static int adjacent_track(int from, int direction, bool automatic)
{
    if (!playlist_count) return -1;
    if (automatic && play_mode == PLAY_REPEAT_ONE) return from;

    if (play_mode == PLAY_SHUFFLE && playlist_count > 1) {
        if (shuffle_size != playlist_count)
            rebuild_shuffle(from);
        /* Keep the cursor aligned with whatever is playing so that stepping
           backwards through the bag is meaningful. */
        if (shuffle_pos >= shuffle_size || shuffle_order[shuffle_pos] != from) {
            for (size_t i = 0; i < shuffle_size; i++)
                if (shuffle_order[i] == from) { shuffle_pos = i; break; }
        }
        if (direction > 0) {
            shuffle_pos++;
            if (shuffle_pos >= shuffle_size) { rebuild_shuffle(-1); shuffle_pos = 0; }
        } else {
            shuffle_pos = shuffle_pos ? shuffle_pos - 1 : shuffle_size - 1;
        }
        return shuffle_order[shuffle_pos];
    }

    return (from + direction + (int)playlist_count) % (int)playlist_count;
}

static void load_track_summary(playlist_track_t *track)
{
    if (track->summary_loaded)
        return;
    track->summary_loaded = true;
    media_metadata_t *meta = &scratch_meta;
    if (media_metadata_read(track->path, meta, true)) {
        if (meta->artist[0])
            snprintf(track->title, sizeof(track->title), "%.30s - %.28s", meta->title, meta->artist);
        else
            snprintf(track->title, sizeof(track->title), "%.62s", meta->title);
        track->duration_ms = meta->duration_ms;
    } else {
        snprintf(track->title, sizeof(track->title), "%s", rg_basename(track->path));
    }
}

/*******************************************************************************
 * Persisted state
 ******************************************************************************/

static void save_resume(bool commit)
{
    if (!media_ready) return;
    media_player_snapshot_t s;
    media_player_get_snapshot(&s);
    if (!s.path[0] || s.state == MEDIA_STOPPED) return;

    uint32_t position = s.position_ms;
    /* Treat "almost finished" as "start over next time". */
    if (s.duration_ms && position + 5000 >= s.duration_ms) position = 0;

    bool same = position == last_saved_position && resume_path && !strcmp(resume_path, s.path);
    if (!same) {
        free(resume_path);
        resume_path = strdup(s.path);
        resume_position = last_saved_position = position;
        rg_settings_set_string(NS_APP, SETTING_MEDIA_PATH, s.path);
        rg_settings_set_number(NS_APP, SETTING_MEDIA_POSITION, position);
    }
    if (commit) {
        rg_settings_commit();
        next_resume_commit = rg_system_timer() + RESUME_COMMIT_INTERVAL_US;
    }
}

static void save_equalizer(void)
{
    char key[24];
    rg_settings_set_number(NS_APP, SETTING_MEDIA_EQ_ON, media_eq_get_enabled());
    rg_settings_set_number(NS_APP, SETTING_MEDIA_EQ_PRESET, media_eq_get_preset());
    for (int i = 0; i < MEDIA_EQ_BANDS; i++) {
        snprintf(key, sizeof(key), SETTING_MEDIA_EQ_GAIN, i);
        rg_settings_set_number(NS_APP, key, media_eq_get_gain(i));
    }
    /* save_resume() bails out when playback is stopped, so it cannot be relied
       on to flush these for us. */
    rg_settings_commit();
}

static void load_equalizer(void)
{
    char key[24];
    int preset = (int)rg_settings_get_number(NS_APP, SETTING_MEDIA_EQ_PRESET, MEDIA_EQ_PRESET_FLAT);
    media_eq_set_preset(preset >= 0 && preset < MEDIA_EQ_PRESET_COUNT ? preset : MEDIA_EQ_PRESET_FLAT);
    /* Custom curves are stored band by band; named presets rebuild themselves
       from the table above, so only reload the gains for a custom curve. */
    if (media_eq_get_preset() == MEDIA_EQ_PRESET_CUSTOM) {
        for (int i = 0; i < MEDIA_EQ_BANDS; i++) {
            snprintf(key, sizeof(key), SETTING_MEDIA_EQ_GAIN, i);
            media_eq_set_gain(i, (int)rg_settings_get_number(NS_APP, key, 0));
        }
        media_eq_set_preset(MEDIA_EQ_PRESET_CUSTOM);
    }
    media_eq_set_enabled(rg_settings_get_number(NS_APP, SETTING_MEDIA_EQ_ON, 0) != 0);
}

/*******************************************************************************
 * Playback control
 ******************************************************************************/

static bool play_playlist_track_at(int index, uint32_t start_ms)
{
    if (index < 0 || index >= (int)playlist_count) return false;
    /* Flush now, while the old stream is still buffered: an SD write once the
       new track is starting would fight it for the card. */
    save_resume(true);

    playlist_track_t *track = &playlist[index];
    /* Read straight into the shared copy: it is what every draw call reads
       from, and it keeps a 1.3 KB struct off the stack. */
    if (!media_metadata_read(track->path, &current_meta, true)) return false;
    media_metadata_t *meta = &current_meta;
    if (!media_player_play(track->path, meta, start_ms)) return false;

    media_ready = true;
    current_track = index;
    last_saved_position = start_ms;
    next_resume_touch = rg_system_timer() + RESUME_TOUCH_INTERVAL_US;

    if (!track->summary_loaded) {
        track->summary_loaded = true;
        if (meta->artist[0]) snprintf(track->title, sizeof(track->title), "%.30s - %.28s", meta->title, meta->artist);
        else snprintf(track->title, sizeof(track->title), "%.62s", meta->title);
        track->duration_ms = meta->duration_ms;
    }

    /* Cover art and lyrics also live on the SD card. Loading them right now
       would stall the reader exactly when the ring buffer is empty, so defer
       until the stream is on its feet. */
    rg_surface_free(player_cover);
    player_cover = NULL;
    has_cover = false;
    media_lyrics_free(&player_lyrics);
    snprintf(artwork_path, sizeof(artwork_path), "%s", track->path);
    artwork_pending = true;
    return true;
}

static bool play_playlist_track(int index) { return play_playlist_track_at(index, 0); }

static bool playlist_append(playlist_track_t **tracks, size_t *count, size_t *capacity, const char *path)
{
    if (!is_media(path) || !rg_storage_exists(path)) return false;
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 16;
        playlist_track_t *resized = realloc(*tracks, next_capacity * sizeof(**tracks));
        if (!resized) return false;
        *tracks = resized; *capacity = next_capacity;
    }
    playlist_track_t *track = &(*tracks)[(*count)++];
    memset(track, 0, sizeof(*track));
    snprintf(track->path, sizeof(track->path), "%s", path);
    return true;
}

static void replace_playlist(playlist_track_t *tracks, size_t count)
{
    free(playlist);
    playlist = tracks;
    playlist_count = count;
    free(shuffle_order);
    shuffle_order = NULL;
    shuffle_size = shuffle_pos = 0;
}

static bool load_m3u(const char *m3u_path)
{
    FILE *file = fopen(m3u_path, "rb");
    if (!file) return false;
    char base[RG_PATH_MAX + 1];
    snprintf(base, sizeof(base), "%s", m3u_path);
    char *slash = strrchr(base, '/');
    if (slash) *slash = 0;

    playlist_track_t *next = NULL;
    size_t count = 0, capacity = 0;
    char line[RG_PATH_MAX + 4];
    while (fgets(line, sizeof(line), file)) {
        char *text = line;
        if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text += 3;
        while (isspace((unsigned char)*text)) text++;
        char *end = text + strlen(text);
        while (end > text && isspace((unsigned char)end[-1])) *--end = 0;
        if (!*text || *text == '#' || strstr(text, "://")) continue;
        for (char *p = text; *p; p++) if (*p == '\\') *p = '/';

        char path[RG_PATH_MAX + 1];
        if (text[0] == '/' && !strncmp(text, RG_STORAGE_ROOT "/", strlen(RG_STORAGE_ROOT) + 1)) {
            snprintf(path, sizeof(path), "%s", text);
        } else if (text[0] == '/') {
            size_t root_length = strlen(RG_STORAGE_ROOT), text_length = strlen(text);
            if (root_length + text_length >= sizeof(path)) continue;
            memcpy(path, RG_STORAGE_ROOT, root_length);
            memcpy(path + root_length, text, text_length + 1);
        } else {
            size_t base_length = strlen(base), text_length = strlen(text);
            if (base_length + 1 + text_length >= sizeof(path)) continue;
            memcpy(path, base, base_length);
            path[base_length] = '/';
            memcpy(path + base_length + 1, text, text_length + 1);
        }
        playlist_append(&next, &count, &capacity, path);
    }
    fclose(file);
    if (!count) { free(next); return false; }

    replace_playlist(next, count);
    int selected = 0;
    uint32_t start = 0;
    if (resume_path) {
        for (size_t i = 0; i < playlist_count; i++) {
            if (!strcmp(resume_path, playlist[i].path)) { selected = i; start = resume_position; break; }
        }
    }
    return play_playlist_track_at(selected, start);
}

static bool play_track(int entry_index)
{
    if (entry_index < 0 || entry_index >= (int)entry_count || entries[entry_index].type != ENTRY_MEDIA) return false;
    size_t count = 0;
    for (size_t i = 0; i < entry_count; i++) if (entries[i].type == ENTRY_MEDIA) count++;
    playlist_track_t *next = calloc(count, sizeof(*next));
    if (!next) return false;

    int selected = -1;
    size_t out = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].type != ENTRY_MEDIA) continue;
        path_for_entry(&entries[i], next[out].path, sizeof(next[out].path));
        if (entries[i].metadata_loaded) {
            next[out].summary_loaded = true;
            next[out].duration_ms = entries[i].metadata.duration_ms;
            if (entries[i].metadata.artist[0])
                snprintf(next[out].title, sizeof(next[out].title), "%.30s - %.28s",
                         entries[i].metadata.title, entries[i].metadata.artist);
            else
                snprintf(next[out].title, sizeof(next[out].title), "%.62s", entries[i].metadata.title);
        }
        if ((int)i == entry_index) selected = out;
        out++;
    }
    if (selected < 0) { free(next); return false; }

    replace_playlist(next, count);
    uint32_t start = resume_path && !strcmp(resume_path, playlist[selected].path) ? resume_position : 0;
    return play_playlist_track_at(selected, start);
}

static const char *play_mode_name(void)
{
    static const char *names[] = {"REPEAT ALL", "REPEAT 1", "SHUFFLE"};
    return names[play_mode % PLAY_MODE_COUNT];
}

static void cycle_play_mode(void)
{
    play_mode = (play_mode + 1) % PLAY_MODE_COUNT;
    if (play_mode == PLAY_SHUFFLE)
        rebuild_shuffle(current_track);
    rg_settings_set_number(NS_APP, SETTING_MEDIA_PLAY_MODE, play_mode);
}

static void cycle_sleep_timer(void)
{
    static const uint16_t choices[] = {0, 15, 30, 45, 60, 90, 120};
    size_t index = 0;
    while (index + 1 < RG_COUNT(choices) && choices[index] != sleep_minutes) index++;
    sleep_minutes = choices[(index + 1) % RG_COUNT(choices)];
    sleep_deadline = sleep_minutes ? rg_system_timer() + (int64_t)sleep_minutes * 60 * 1000000 : 0;
    rg_settings_set_number(NS_APP, SETTING_MEDIA_SLEEP, sleep_minutes);
}

static void load_pending_artwork(void)
{
    if (!artwork_pending || !artwork_path[0])
        return;
    artwork_pending = false;
    /* A JPEG decode follows straight after this, so nothing large goes on the
       stack here. */
    media_metadata_t *meta = &scratch_meta;
    if (media_metadata_read(artwork_path, meta, false))
        player_cover = media_metadata_load_cover(artwork_path, meta, gui.width, gui.height);
    has_cover = player_cover != NULL;
    media_lyrics_load(artwork_path, &player_lyrics);
}

static void playback_service(void)
{
    if (!media_ready) return;
    int64_t now = rg_system_timer();
    media_player_snapshot_t s;
    media_player_get_snapshot(&s);

    if (sleep_deadline && now >= sleep_deadline) {
        save_resume(true);
        media_player_stop();
        sleep_deadline = 0;
        sleep_minutes = 0;
        rg_settings_set_number(NS_APP, SETTING_MEDIA_SLEEP, 0);
        rg_settings_commit();
        return;
    }

    if (s.state == MEDIA_PLAYING || s.state == MEDIA_PAUSED) {
        if (now >= next_resume_touch) {
            bool commit = now >= next_resume_commit;
            save_resume(commit);
            next_resume_touch = now + RESUME_TOUCH_INTERVAL_US;
        }
    }

#if defined(ESP_PLATFORM) && defined(RG_GPIO_HEADPHONE_DETECT)
    bool raw = gpio_get_level(RG_GPIO_HEADPHONE_DETECT) == RG_HEADPHONE_DETECT_LEVEL;
    if (raw != headphone_raw) { headphone_raw = raw; headphone_changed_at = now; }
    if (raw != headphone_present && now - headphone_changed_at >= 150000) {
        headphone_present = raw;
        if (!raw && (s.state == MEDIA_PLAYING || s.state == MEDIA_BUFFERING)) {
            media_player_set_paused(true);
            paused_for_headphones = true;
        } else if (raw && paused_for_headphones) {
            media_player_set_paused(false);
            paused_for_headphones = false;
        }
    }
#endif
}

static bool play_finished_track(void)
{
    if (!media_ready) return false;
    if (!media_player_take_finished()) return false;
    int index = adjacent_track(current_track, 1, true);
    if (index < 0) return false;
    return play_playlist_track(index);
}

/*******************************************************************************
 * Low level drawing
 *
 * The generic rectangle and text helpers stage a scratch buffer per call, which
 * is fine for a menu but far too much ceremony for a visualiser running at
 * 30 fps. Anything that draws per-pixel geometry writes to the surface here.
 ******************************************************************************/

static rg_surface_t *backdrop;
static int header_bottom, content_top, content_bottom, meta_top, progress_top, footer_top;

static inline uint16_t *surface_row(rg_surface_t *surface, int y)
{
    return (uint16_t *)((uint8_t *)surface->data + surface->offset + y * surface->stride);
}

/* RGB565 channel-wise interpolation. `mix` is 0-255, 0 selects `a`. */
static uint16_t blend565(uint16_t a, uint16_t b, int mix)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + ((br - ar) * mix >> 8);
    int g = ag + ((bg - ag) * mix >> 8);
    int bl = ab + ((bb - ab) * mix >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void clip_rect(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > gui.width) *w = gui.width - *x;
    if (*y + *h > gui.height) *h = gui.height - *y;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    clip_rect(&x, &y, &w, &h);
    if (!gui.surface || w <= 0 || h <= 0)
        return;
    for (int row = 0; row < h; row++) {
        uint16_t *line = surface_row(gui.surface, y + row) + x;
        for (int i = 0; i < w; i++)
            line[i] = color;
    }
}

/* Vertical gradient from `top` to `bottom`, used for the bars and the scrim. */
static void fill_gradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    int full_height = h;
    int skipped = y < 0 ? -y : 0;
    clip_rect(&x, &y, &w, &h);
    if (!gui.surface || w <= 0 || h <= 0)
        return;
    for (int row = 0; row < h; row++) {
        int mix = full_height > 1 ? (skipped + row) * 255 / (full_height - 1) : 0;
        uint16_t color = blend565(top, bottom, mix);
        uint16_t *line = surface_row(gui.surface, y + row) + x;
        for (int i = 0; i < w; i++)
            line[i] = color;
    }
}

/*******************************************************************************
 * Presentation
 *
 * rg_display_submit() hands the whole surface to the display task, which lives
 * on RG_TASK_AFFINITY_IO -- the same core as the audio writer, the decoder and
 * the SD reader. Every frame it byte-swaps and hashes all 150k pixels, reading
 * the surface straight out of PSRAM, and at 30 fps that is enough CPU and
 * enough PSRAM bandwidth to make the decoder miss its deadline.
 *
 * The player therefore presents its own dirty rectangles with
 * rg_display_write_rect(), which performs the transfer on the calling task.
 * That puts all display work on the UI core and only pushes pixels that
 * actually changed, which for a visualiser frame is a fraction of the screen.
 ******************************************************************************/

#define MAX_DIRTY_RECTS 6
static rg_rect_t dirty_rects[MAX_DIRTY_RECTS];
static int dirty_count;

static void mark_dirty(int x, int y, int w, int h)
{
    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0)
        return;

    /* Merge into an overlapping or adjacent rect; a handful of bands covers
       every layout we draw, and merging keeps the transfer count low. */
    for (int i = 0; i < dirty_count; i++) {
        rg_rect_t *r = &dirty_rects[i];
        if (x <= r->left + r->width && r->left <= x + w && y <= r->top + r->height && r->top <= y + h) {
            int right = RG_MAX(r->left + r->width, x + w);
            int bottom = RG_MAX(r->top + r->height, y + h);
            r->left = RG_MIN(r->left, x);
            r->top = RG_MIN(r->top, y);
            r->width = right - r->left;
            r->height = bottom - r->top;
            return;
        }
    }

    if (dirty_count == MAX_DIRTY_RECTS) {
        /* Out of slots: collapse everything into one bounding box. */
        rg_rect_t *r = &dirty_rects[0];
        for (int i = 1; i < dirty_count; i++) {
            int right = RG_MAX(r->left + r->width, dirty_rects[i].left + dirty_rects[i].width);
            int bottom = RG_MAX(r->top + r->height, dirty_rects[i].top + dirty_rects[i].height);
            r->left = RG_MIN(r->left, dirty_rects[i].left);
            r->top = RG_MIN(r->top, dirty_rects[i].top);
            r->width = right - r->left;
            r->height = bottom - r->top;
        }
        dirty_count = 1;
        mark_dirty(x, y, w, h);
        return;
    }

    dirty_rects[dirty_count++] = (rg_rect_t){x, y, w, h};
}

static void flush_dirty(void)
{
    if (!gui.surface)
        dirty_count = 0;
    if (!dirty_count)
        return;

    /* One wait for the display task (which should be idle here anyway), then
       every band goes out back to back. */
    rg_display_sync(true);
    for (int i = 0; i < dirty_count; i++) {
        const rg_rect_t *r = &dirty_rects[i];
        const uint8_t *pixels = (const uint8_t *)gui.surface->data + gui.surface->offset +
                                (size_t)r->top * gui.surface->stride + (size_t)r->left * 2;
        rg_display_write_rect(r->left, r->top, r->width, r->height, gui.surface->stride,
                              (const uint16_t *)pixels, RG_DISPLAY_WRITE_NOSYNC);
    }
    dirty_count = 0;
}

static void plot_column(int x, int top, int bottom, uint16_t color)
{
    if (!gui.surface || x < 0 || x >= gui.width)
        return;
    if (top < 0) top = 0;
    if (bottom >= gui.height) bottom = gui.height - 1;
    for (int y = top; y <= bottom; y++)
        surface_row(gui.surface, y)[x] = color;
}

/* Darkens a horizontal band of the backdrop so overlaid text stays readable
   without washing out the artwork the way a flat dim does. */
static void apply_scrim(rg_surface_t *surface, int y, int h, int strength_top, int strength_bottom)
{
    if (y < 0) { h += y; y = 0; }
    if (y + h > surface->height) h = surface->height - y;
    if (h <= 0)
        return;
    for (int row = 0; row < h; row++) {
        int strength = strength_top + (strength_bottom - strength_top) * row / RG_MAX(1, h - 1);
        if (strength <= 0)
            continue;
        uint16_t *line = surface_row(surface, y + row);
        for (int x = 0; x < surface->width; x++)
            line[x] = blend565(line[x], C_BLACK, RG_MIN(255, strength));
    }
}

/* The backdrop is expensive (a scaled cover plus the scrim pass) and it never
   changes within a track, so it is rendered once and then used as the source
   for partial repaints. */
static void build_backdrop(void)
{
    if (backdrop && (backdrop->width != gui.width || backdrop->height != gui.height)) {
        rg_surface_free(backdrop);
        backdrop = NULL;
    }
    if (!backdrop)
        backdrop = rg_surface_create(gui.width, gui.height, RG_PIXEL_565_LE, MEM_SLOW | MEM_NOPANIC);
    if (!backdrop)
        return;

    if (player_cover) {
        /* Full bleed artwork, left sharp through the middle of the screen. */
        rg_surface_copy(player_cover, NULL, backdrop, NULL, true);
        apply_scrim(backdrop, 0, gui.height * 22 / 100, 205, 0);
        apply_scrim(backdrop, gui.height * 42 / 100, gui.height - gui.height * 42 / 100, 0, 225);
    } else {
        for (int y = 0; y < gui.height; y++) {
            int mix = y * 255 / RG_MAX(1, gui.height - 1);
            uint16_t color = blend565(MEDIA_RGB(16, 22, 15), MEDIA_RGB(4, 4, 3), mix);
            uint16_t *line = surface_row(backdrop, y);
            for (int x = 0; x < gui.width; x++)
                line[x] = color;
        }
    }

    /* The full size cover is only ever a source for the backdrop, so releasing
       it here gives roughly 300 KB of PSRAM back for every track. */
    rg_surface_free(player_cover);
    player_cover = NULL;
}

/* Load whatever artwork and lyrics the current track has, then fold the cover
   into the cached backdrop. Both steps read the SD card, so callers must only
   do this once playback is no longer starving for data. */
static void refresh_artwork(void)
{
    load_pending_artwork();
    build_backdrop();
}

/* Repaint a region of the screen buffer from the cached backdrop. This is what
   lets the player animate at 30 fps without touching the rest of the screen. */
static void restore_rect(int x, int y, int w, int h)
{
    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    if (!backdrop || !gui.surface) {
        fill_rect(x, y, w, h, C_BLACK);
        return;
    }
    for (int row = 0; row < h; row++) {
        const uint8_t *src = (const uint8_t *)backdrop->data + backdrop->offset + (y + row) * backdrop->stride + x * 2;
        uint8_t *dst = (uint8_t *)gui.surface->data + gui.surface->offset + (y + row) * gui.surface->stride + x * 2;
        memcpy(dst, src, (size_t)w * 2);
    }
}

static int text_width(const char *text, uint32_t flags)
{
    return rg_gui_draw_text(0, 0, 0, text, C_WHITE, C_TRANSPARENT, flags | RG_TEXT_DUMMY_DRAW).width;
}

/* Set whenever a marquee had to scroll during the last frame, so the player
   only schedules animation frames when something is actually moving. */
static bool marquee_active;

/* Character based marquee. Pixel scrolling is not possible because the text
   renderer clips to the box and cannot draw at a negative offset. */
static void marquee(int x, int y, int width, const char *text, rg_color_t color, uint32_t flags, int step)
{
    if (!text || !text[0])
        return;
    if (text_width(text, flags) <= width) {
        rg_gui_draw_text_bidi(x, y, width, text, color, C_TRANSPARENT, flags | RG_TEXT_ALIGN_CENTER);
        return;
    }
    marquee_active = true;
    static char scratch[512];
    const char *separator = "   *   ";
    size_t len = strlen(text), sep = strlen(separator);
    size_t period = len + sep;
    size_t offset = period ? (size_t)step % period : 0;

    /* Advance by whole codepoints so multibyte text never gets cut in half. */
    while (offset < len && ((unsigned char)text[offset] & 0xC0) == 0x80) offset++;

    size_t used = 0;
    for (size_t i = offset; i < len && used + 1 < sizeof(scratch); i++) scratch[used++] = text[i];
    for (size_t i = 0; i < sep && used + 1 < sizeof(scratch); i++) scratch[used++] = separator[i];
    for (size_t i = 0; i < len && used + 1 < sizeof(scratch); i++) scratch[used++] = text[i];
    scratch[used] = 0;
    rg_gui_draw_text_bidi(x, y, width, scratch, color, C_TRANSPARENT, flags | RG_TEXT_ALIGN_LEFT);
}

/*******************************************************************************
 * Visualisers (analysis runs here, not on the decoder thread)
 ******************************************************************************/

/* Roughly third-octave spacing from 40 Hz to 16 kHz. */
static const uint16_t band_frequency[SPECTRUM_BANDS] = {
    40,   55,   75,  100,  130,  175,  230,  310,
    410,  540,  720,  950, 1250, 1650, 2200, 2900,
    3800, 5000, 6500, 8000, 9500, 11000, 13000, 15000,
};
static float band_coefficient[SPECTRUM_BANDS];
static uint32_t coefficient_rate;
static float band_level[SPECTRUM_BANDS];
static float band_peak[SPECTRUM_BANDS];
static float channel_level[2], channel_peak[2];
static int16_t scope_frames[MEDIA_SCOPE_FRAMES * 2];
static float analysis_window[MEDIA_SCOPE_FRAMES];
static float analysis_buffer[MEDIA_SCOPE_FRAMES];
static size_t analysis_length;
static bool window_ready;

static float decibel_scale(float magnitude, float reference)
{
    /* Maps roughly -60 dBFS..0 dBFS onto 0..100. */
    float value = 100.0f * (1.0f + log10f(RG_MAX(magnitude, 1e-5f) * reference + 1e-6f) / 3.0f);
    return RG_MIN(100.0f, RG_MAX(0.0f, value));
}

/* Returns true while the display is still changing, so a paused or stopped
   player settles to zero and then stops asking for frames entirely. */
static bool analyze_audio(bool active)
{
    uint32_t rate = 0;
    size_t count = active ? media_player_read_scope(scope_frames, RG_COUNT(analysis_window), &rate) : 0;
    analysis_length = count;

    if (count < 64 || !rate) {
        bool moving = false;
        for (int i = 0; i < SPECTRUM_BANDS; i++) {
            band_level[i] = band_level[i] > 0.05f ? band_level[i] * 0.80f : 0.0f;
            band_peak[i] = RG_MAX(band_level[i], band_peak[i] - 2.0f);
            if (band_level[i] > 0.0f || band_peak[i] > 0.0f) moving = true;
        }
        for (int c = 0; c < 2; c++) {
            channel_level[c] = channel_level[c] > 0.05f ? channel_level[c] * 0.80f : 0.0f;
            channel_peak[c] = RG_MAX(channel_level[c], channel_peak[c] - 2.0f);
            if (channel_level[c] > 0.0f || channel_peak[c] > 0.0f) moving = true;
        }
        return moving;
    }

    if (!window_ready) {
        for (size_t i = 0; i < RG_COUNT(analysis_window); i++)
            analysis_window[i] = 0.5f - 0.5f * cosf(2.0f * MEDIA_PI * i / (RG_COUNT(analysis_window) - 1));
        window_ready = true;
    }
    if (rate != coefficient_rate) {
        for (int i = 0; i < SPECTRUM_BANDS; i++)
            band_coefficient[i] = 2.0f * cosf(2.0f * MEDIA_PI * band_frequency[i] / rate);
        coefficient_rate = rate;
    }

    /* Per-channel RMS for the VU meter, and a windowed mono mix for the bands.
       Windowing keeps neighbouring bands from bleeding into each other. */
    float scale = (float)RG_COUNT(analysis_window) / count;
    float sum[2] = {0.0f, 0.0f};
    for (size_t i = 0; i < count; i++) {
        float left = scope_frames[i * 2] / 32768.0f;
        float right = scope_frames[i * 2 + 1] / 32768.0f;
        sum[0] += left * left;
        sum[1] += right * right;
        analysis_buffer[i] = (left + right) * 0.5f * analysis_window[(size_t)(i * scale)];
    }
    for (int c = 0; c < 2; c++) {
        float target = decibel_scale(sqrtf(sum[c] / count), 8.0f);
        if (target > channel_level[c]) channel_level[c] += (target - channel_level[c]) * 0.7f;
        else channel_level[c] += (target - channel_level[c]) * 0.15f;
        channel_peak[c] = channel_level[c] > channel_peak[c] ? channel_level[c]
                                                             : RG_MAX(channel_level[c], channel_peak[c] - 1.2f);
    }

    for (int band = 0; band < SPECTRUM_BANDS; band++) {
        float target = 0.0f;
        if (band_frequency[band] < rate / 2) {
            float coefficient = band_coefficient[band];
            float q1 = 0.0f, q2 = 0.0f;
            for (size_t i = 0; i < count; i++) {
                float q0 = analysis_buffer[i] + coefficient * q1 - q2;
                q2 = q1;
                q1 = q0;
            }
            float power = q1 * q1 + q2 * q2 - coefficient * q1 * q2;
            /* A gentle tilt lifts the top end, which always reads quieter on a
               constant-Q bank fed with real music. */
            float tilt = 1.0f + band * 0.06f;
            target = decibel_scale(sqrtf(RG_MAX(0.0f, power)) / count, 40.0f * tilt);
        }
        /* Fast attack so transients register, slow release so it stays smooth. */
        if (target > band_level[band]) band_level[band] += (target - band_level[band]) * 0.65f;
        else band_level[band] += (target - band_level[band]) * 0.22f;
        band_peak[band] = band_level[band] > band_peak[band] ? band_level[band]
                                                            : RG_MAX(band_level[band], band_peak[band] - 1.6f);
    }
    return true;
}

/* Bar colour runs cool at the bottom to hot at the top, and the hue also
   shifts across the spectrum so the bass end reads differently from the highs. */
static uint16_t bar_color(int band, int row, int rows)
{
    uint16_t base = band < SPECTRUM_BANDS / 3 ? MEDIA_RGB(0, 140, 24)
                  : band < SPECTRUM_BANDS * 2 / 3 ? MEDIA_RGB(0, 200, 20)
                                                  : MEDIA_RGB(0, 230, 12);
    uint16_t hot = band < SPECTRUM_BANDS / 3 ? MEDIA_RGB(255, 60, 8) : MEDIA_RGB(255, 200, 4);
    int mix = rows > 1 ? row * 255 / (rows - 1) : 0;
    return blend565(base, hot, mix);
}

static const char *spectrum_style_name(spectrum_style_t style)
{
    static const char *names[SPECTRUM_STYLE_COUNT] = {"BARS", "BLOCKS", "MIRROR", "WAVE"};
    return names[style % SPECTRUM_STYLE_COUNT];
}

/* Gradient columns with a floating peak cap and a dimmed reflection. */
static void spectrum_bars(int margin, int bar_width, int gap, int base, int max_height, int mirror_height)
{
    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        int x = margin + i * (bar_width + gap);
        int height = RG_MAX(1, (int)(max_height * band_level[i] / 100.0f));
        int peak = RG_MAX(1, (int)(max_height * band_peak[i] / 100.0f));

        fill_rect(x, base - max_height, bar_width, max_height - height, MEDIA_RGB(18, 22, 30));

        /* One gradient pass instead of a fill per row. The endpoints are taken
           at absolute heights so a quiet bar stays green rather than being
           rescaled to the full ramp. */
        uint16_t low = bar_color(i, 0, max_height);
        uint16_t high = bar_color(i, height - 1, max_height);
        fill_gradient(x, base - height, bar_width, height, high, low);
        fill_rect(x, base - peak - 3, bar_width, 2, C_WHITE);

        int reflected = RG_MIN(mirror_height, height);
        if (reflected > 0)
            fill_gradient(x, base + 2, bar_width, reflected, blend565(low, C_BLACK, 120), C_BLACK);
    }
}

/* Classic segmented LED ladder: discrete cells with a dark gap between them. */
static void spectrum_blocks(int margin, int bar_width, int gap, int base, int max_height, int mirror_height)
{
    const int cell = 6, spacing = 2;
    int cells = max_height / (cell + spacing);
    if (cells < 2)
        cells = 2;

    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        int x = margin + i * (bar_width + gap);
        int lit = (int)(cells * band_level[i] / 100.0f + 0.5f);
        int peak_cell = (int)(cells * band_peak[i] / 100.0f + 0.5f);

        for (int c = 0; c < cells; c++) {
            int y = base - (c + 1) * (cell + spacing) + spacing;
            uint16_t color;
            if (c < lit)
                color = bar_color(i, c * max_height / cells, max_height);
            else if (c == peak_cell - 1)
                color = C_WHITE;
            else
                /* Unlit cells stay faintly visible so the ladder reads as a
                   scale rather than as empty space. */
                color = MEDIA_RGB(22, 26, 36);
            fill_rect(x, y, bar_width, cell, color);
        }

        int reflected = RG_MIN(mirror_height / (cell + spacing), lit);
        for (int c = 0; c < reflected; c++) {
            uint16_t color = bar_color(i, c * max_height / cells, max_height);
            fill_rect(x, base + 2 + c * (cell + spacing), bar_width, cell,
                      blend565(color, C_BLACK, 130 + c * 30));
        }
    }
}

/* Symmetric bars growing out of a centre line. */
static void spectrum_mirror(int margin, int bar_width, int gap, int centre, int half)
{
    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        int x = margin + i * (bar_width + gap);
        int height = RG_MAX(1, (int)(half * band_level[i] / 100.0f));
        int peak = RG_MAX(1, (int)(half * band_peak[i] / 100.0f));
        uint16_t low = bar_color(i, 0, half);
        uint16_t high = bar_color(i, height - 1, half);

        fill_rect(x, centre - half, bar_width, half - height, MEDIA_RGB(18, 22, 30));
        fill_rect(x, centre + height, bar_width, half - height, MEDIA_RGB(18, 22, 30));
        fill_gradient(x, centre - height, bar_width, height, high, low);
        fill_gradient(x, centre, bar_width, height, low, high);
        fill_rect(x, centre - peak - 2, bar_width, 2, C_WHITE);
        fill_rect(x, centre + peak, bar_width, 2, C_WHITE);
    }
    fill_rect(margin, centre - 1, SPECTRUM_BANDS * (bar_width + gap) - gap, 1, MEDIA_RGB(120, 140, 170));
}

/* Filled envelope: the band tops joined into a continuous skyline. */
static void spectrum_wave(int margin, int usable, int base, int max_height)
{
    int previous = -1;
    for (int x = 0; x < usable; x++) {
        /* Linear interpolation between adjacent band tops gives a smooth
           silhouette instead of 24 hard steps. */
        float position = (float)x * (SPECTRUM_BANDS - 1) / RG_MAX(1, usable - 1);
        int index = (int)position;
        float fraction = position - index;
        float level = band_level[index];
        if (index + 1 < SPECTRUM_BANDS)
            level += (band_level[index + 1] - level) * fraction;

        int height = RG_MAX(1, (int)(max_height * level / 100.0f));
        uint16_t top = bar_color(index, height - 1, max_height);
        fill_gradient(margin + x, base - height, 1, height, top, blend565(top, C_BLACK, 190));

        /* Bright crest line, joined to the previous column. */
        int y = base - height;
        int from = previous < 0 ? y : RG_MIN(previous, y);
        int to = previous < 0 ? y : RG_MAX(previous, y);
        plot_column(margin + x, from, to, C_WHITE);
        previous = y;
    }
}

static void draw_spectrum(void)
{
    int margin = RG_MAX(6, gui.width / 60);
    int usable = gui.width - margin * 2;
    int gap = gui.width >= 400 ? 2 : 1;
    int bar_width = (usable - (SPECTRUM_BANDS - 1) * gap) / SPECTRUM_BANDS;
    if (bar_width < 2)
        return;

    /* Two extra rows keep the reflection inside the content region: it starts
       two pixels below the baseline. */
    int available = content_bottom - content_top - 20;
    int mirror_height = available / 4;
    int max_height = available - mirror_height;
    int base = content_top + 18 + max_height;
    if (max_height < 12)
        return;

    switch (spectrum_style) {
    case SPECTRUM_STYLE_BLOCKS:
        spectrum_blocks(margin, bar_width, gap, base, max_height, mirror_height);
        break;
    case SPECTRUM_STYLE_MIRROR:
        spectrum_mirror(margin, bar_width, gap, content_top + 18 + available / 2, available / 2 - 2);
        break;
    case SPECTRUM_STYLE_WAVE:
        spectrum_wave(margin, usable, base, max_height);
        break;
    default:
        spectrum_bars(margin, bar_width, gap, base, max_height, mirror_height);
        break;
    }

    if (spectrum_style != SPECTRUM_STYLE_MIRROR)
        fill_rect(margin, base, usable, 1, MEDIA_RGB(90, 110, 140));

    char title[48];
    snprintf(title, sizeof(title), "SPECTRUM  -  %s", spectrum_style_name(spectrum_style));
    rg_gui_draw_text(0, content_top + 2, gui.width, title, C_AQUA, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

static void draw_vu_channel(const char *label, int y, int height, float level, float peak)
{
    int label_width = 22;
    int x = 14 + label_width;
    int width = gui.width - x - 14;
    if (width < 40)
        return;

    rg_gui_draw_text(14, y + height / 2 - 7, label_width, label, C_SILVER, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    fill_rect(x, y, width, height, MEDIA_RGB(16, 20, 28));

    /* Three zones rather than a fill per pixel column: green up to roughly
       -12 dB, amber to -3 dB, red above. */
    int filled = (int)(width * level / 100.0f);
    int green_end = RG_MIN(filled, width * 70 / 100);
    int amber_end = RG_MIN(filled, width * 88 / 100);
    fill_rect(x, y, green_end, height, MEDIA_RGB(0, 210, 40));
    fill_rect(x + green_end, y, amber_end - green_end, height, MEDIA_RGB(255, 190, 0));
    fill_rect(x + amber_end, y, filled - amber_end, height, MEDIA_RGB(255, 45, 30));
    int peak_x = x + RG_MIN(width - 2, (int)(width * peak / 100.0f));
    fill_rect(peak_x, y - 1, 2, height + 2, C_WHITE);
    /* Scale ticks. */
    for (int p = 10; p < 100; p += 10)
        fill_rect(x + width * p / 100, y + height, 1, 3, MEDIA_RGB(80, 95, 120));
}

static void draw_vu(void)
{
    int middle = (content_top + content_bottom) / 2;
    int height = RG_MIN(28, (content_bottom - content_top) / 5);
    rg_gui_draw_text(0, content_top + 2, gui.width, "LEVEL METER", C_AQUA, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    draw_vu_channel("L", middle - height - 14, height, channel_level[0], channel_peak[0]);
    draw_vu_channel("R", middle + 6, height, channel_level[1], channel_peak[1]);
    rg_gui_draw_text(0, middle + height + 20, gui.width, "-60      -40      -20     -12   -6   0 dB", C_SILVER,
                     C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

static void draw_waveform(void)
{
    int top = content_top + 20;
    int height = content_bottom - top - 6;
    int middle = top + height / 2;
    rg_gui_draw_text(0, content_top + 2, gui.width, "WAVEFORM", C_AQUA, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    if (height < 16 || analysis_length < 8) {
        rg_gui_draw_text(0, middle, gui.width, "No signal", C_SILVER, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
        return;
    }

    fill_rect(8, middle, gui.width - 16, 1, MEDIA_RGB(50, 62, 84));
    int columns = gui.width - 16;
    int amplitude = height / 2 - 2;
    int previous[2] = {middle, middle};
    for (int x = 0; x < columns; x++) {
        size_t index = (size_t)x * analysis_length / columns;
        for (int channel = 0; channel < 2; channel++) {
            int value = middle - (scope_frames[index * 2 + channel] * amplitude) / 32768;
            /* Join consecutive samples so the trace stays continuous instead of
               breaking into dots on steep slopes. */
            plot_column(8 + x, RG_MIN(previous[channel], value), RG_MAX(previous[channel], value),
                        channel ? MEDIA_RGB(0, 190, 255) : MEDIA_RGB(0, 255, 150));
            previous[channel] = value;
        }
    }
}

/*******************************************************************************
 * Equalizer page
 ******************************************************************************/

static void draw_equalizer(int selected)
{
    char line[96];
    bool on = media_eq_get_enabled();

    snprintf(line, sizeof(line), "EQUALIZER  -  %s  -  %s", media_eq_preset_name(media_eq_get_preset()),
             on ? "ON" : "OFF");
    rg_gui_draw_text(0, content_top + 2, gui.width, line, on ? C_AQUA : C_GRAY, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);

    int top = content_top + 22;
    int height = content_bottom - top - 20;
    if (height < 40)
        return;
    int middle = top + height / 2;
    int slot = gui.width / MEDIA_EQ_BANDS;
    int bar_width = RG_MIN(28, slot / 2);

    /* Zero line and +/- 12 dB guides. */
    fill_rect(10, middle, gui.width - 20, 1, MEDIA_RGB(120, 140, 170));
    fill_rect(10, top, gui.width - 20, 1, MEDIA_RGB(45, 55, 72));
    fill_rect(10, top + height, gui.width - 20, 1, MEDIA_RGB(45, 55, 72));

    for (int band = 0; band < MEDIA_EQ_BANDS; band++) {
        int centre = slot * band + slot / 2;
        int x = centre - bar_width / 2;
        int gain = media_eq_get_gain(band);
        int extent = (height / 2) * gain / MEDIA_EQ_GAIN_MAX;
        uint16_t color = !on ? MEDIA_RGB(90, 100, 115)
                       : gain > 0 ? MEDIA_RGB(0, 200, 255)
                       : gain < 0 ? MEDIA_RGB(255, 130, 40)
                                  : MEDIA_RGB(140, 155, 180);

        if (extent > 0)
            fill_gradient(x, middle - extent, bar_width, extent, color, blend565(color, C_BLACK, 120));
        else if (extent < 0)
            fill_gradient(x, middle, bar_width, -extent, blend565(color, C_BLACK, 120), color);
        /* Always draw a handle so a flat band is still visible and selectable. */
        fill_rect(x, middle - extent - 2, bar_width, 4, band == selected ? C_WHITE : color);

        if (band == selected) {
            fill_rect(x - 3, top, 1, height, C_WHITE);
            fill_rect(x + bar_width + 2, top, 1, height, C_WHITE);
        }

        char label[16];
        uint16_t frequency = media_eq_frequencies[band];
        if (frequency >= 1000) snprintf(label, sizeof(label), "%uk", frequency / 1000);
        else snprintf(label, sizeof(label), "%u", frequency);
        rg_gui_draw_text(centre - slot / 2, top + height + 3, slot, label, C_SILVER, C_TRANSPARENT,
                         RG_TEXT_ALIGN_CENTER);
        snprintf(label, sizeof(label), "%+d", gain);
        /* Keep the value beside its handle but inside the graph, otherwise a
           full boost pushes the text up into the page title. */
        int label_y = extent >= 0 ? middle - extent - 18 : middle - extent + 4;
        label_y = RG_MIN(RG_MAX(label_y, top + 1), top + height - 15);
        rg_gui_draw_text(centre - slot / 2, label_y, slot, label,
                         band == selected ? C_WHITE : C_LIGHT_CYAN, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    }
}

/*******************************************************************************
 * Text pages
 ******************************************************************************/

static void draw_lyrics(const media_player_snapshot_t *s)
{
    if (!player_lyrics.count) {
        const char *text = current_meta.lyrics[0] ? current_meta.lyrics
                                                 : "No synchronized lyrics\nAdd a UTF-8 .lrc file beside this track.";
        rg_gui_draw_text_bidi(12, content_top + 20, gui.width - 24, text, C_WHITE, C_TRANSPARENT,
                              RG_TEXT_ALIGN_CENTER | RG_TEXT_MULTILINE);
        return;
    }
    int active = media_lyrics_find(&player_lyrics, s->position_ms);
    int line_height = rg_gui_get_font_height() + 4;
    int centre = (content_top + content_bottom) / 2 - 8;
    int span = (content_bottom - content_top) / (2 * line_height);
    for (int rel = -span; rel <= span; rel++) {
        int i = active + rel;
        if (i < 0 || i >= (int)player_lyrics.count) continue;
        rg_color_t color = rel == 0 ? C_AQUA : (abs(rel) == 1 ? C_WHITE : C_GRAY);
        uint32_t flags = RG_TEXT_ALIGN_CENTER | (rel == 0 ? RG_TEXT_BIGGER : 0);
        rg_gui_draw_text_bidi(10, centre + rel * line_height, gui.width - 20, player_lyrics.lines[i].text, color,
                              C_TRANSPARENT, flags);
    }
}

static void draw_details(const media_player_snapshot_t *s)
{
    char line[192], size[32];
    snprintf(size, sizeof(size), "%.2f MB", current_meta.audio_size / 1048576.0f);
    int y = content_top + 4, h = rg_gui_get_font_height() + 2;
#define DETAIL(label, value) do { \
        snprintf(line, sizeof(line), "%s: %s", label, (value)[0] ? (value) : "-"); \
        rg_gui_draw_text_bidi(14, y, gui.width - 28, line, C_WHITE, C_TRANSPARENT, 0); y += h; } while (0)
    DETAIL("Title", current_meta.title);
    DETAIL("Artist", current_meta.artist);
    DETAIL("Album", current_meta.album);
    DETAIL("Genre", current_meta.genre);
    DETAIL("Year", current_meta.year);
    DETAIL("Track", current_meta.track);
#undef DETAIL

    snprintf(line, sizeof(line), "%s  |  %lu kbps  |  %lu Hz  |  %s  |  %s", media_format_name(s->format),
             (unsigned long)((s->bitrate ? s->bitrate : current_meta.bitrate) / 1000),
             (unsigned long)(s->sample_rate ? s->sample_rate : current_meta.sample_rate),
             s->channels == 1 ? "Mono" : "Stereo", size);
    rg_gui_draw_text(14, y, gui.width - 28, line, C_LIGHT_CYAN, C_TRANSPARENT, 0);
    y += h;

    snprintf(line, sizeof(line), "Buffer %u%%  |  Underruns %lu  |  Bad frames %lu  |  Read errors %lu  |  %s",
             (unsigned)s->buffer_fill, (unsigned long)s->underruns, (unsigned long)s->decode_errors,
             (unsigned long)s->read_errors, s->seekable ? "Seekable" : "No seek");
    rg_gui_draw_text(14, y, gui.width - 28, line, C_SLATE_GRAY, C_TRANSPARENT, 0);
    y += h;

    snprintf(line, sizeof(line), "EQ %s (%s)  |  Cover %s  |  Lyrics %u lines",
             media_eq_get_enabled() ? "on" : "off", media_eq_preset_name(media_eq_get_preset()),
             has_cover ? "yes" : "no", (unsigned)player_lyrics.count);
    rg_gui_draw_text(14, y, gui.width - 28, line, C_SLATE_GRAY, C_TRANSPARENT, 0);
}

/* Returns true when rows are still waiting for their tags to be read, so the
   caller knows to schedule another frame. */
static bool draw_queue(int selection)
{
    int line_height = rg_gui_get_font_height() + 3;
    int rows = (content_bottom - content_top - 22) / line_height;
    if (rows < 1)
        return false;
    int first = selection - rows / 2;
    if (first > (int)playlist_count - rows) first = (int)playlist_count - rows;
    if (first < 0) first = 0;
    int summary_budget = QUEUE_SUMMARIES_PER_FRAME;
    bool pending = false;

    char header[64];
    snprintf(header, sizeof(header), "QUEUE  %d/%d", playlist_count ? selection + 1 : 0, (int)playlist_count);
    rg_gui_draw_text(0, content_top + 2, gui.width, header, C_AQUA, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);

    for (int row = 0; row < rows; row++) {
        int index = first + row;
        if (index >= (int)playlist_count) break;
        /* Summaries are read lazily so opening the queue never blocks on a
           whole folder worth of tag parsing. */
        if (!playlist[index].summary_loaded) {
            if (summary_budget > 0) {
                summary_budget--;
                load_track_summary(&playlist[index]);
            } else {
                pending = true;
                if (!playlist[index].title[0])
                    snprintf(playlist[index].title, sizeof(playlist[index].title), "%s",
                             rg_basename(playlist[index].path));
            }
        }
        int y = content_top + 22 + row * line_height;
        bool selected = index == selection;
        bool playing = index == current_track;
        rg_color_t fg = selected ? C_BLACK : playing ? C_AQUA : C_WHITE;
        if (selected)
            fill_rect(8, y, gui.width - 16, line_height, C_AQUA);
        else if (playing)
            fill_rect(8, y, gui.width - 16, line_height, MEDIA_RGB(10, 30, 50));

        char line[128], duration[16];
        media_format_time(playlist[index].duration_ms, duration, sizeof(duration));
        snprintf(line, sizeof(line), "%c %2d. %.60s", playing ? '>' : ' ', index + 1, playlist[index].title);
        rg_gui_draw_text_bidi(10, y + 1, gui.width - 76, line, fg, C_TRANSPARENT, 0);
        rg_gui_draw_text(gui.width - 64, y + 1, 56, duration, fg, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
    }
    return pending;
}

/*******************************************************************************
 * Chrome: header, hero block, progress, footer
 ******************************************************************************/

static const char *page_name(page_t page)
{
    static const char *names[PAGE_COUNT] = {
        "NOW PLAYING", "SPECTRUM", "LEVEL METER", "WAVEFORM", "LYRICS", "EQUALIZER", "QUEUE", "DETAILS",
    };
    return names[page % PAGE_COUNT];
}

static void draw_header(const media_player_snapshot_t *s, page_t page)
{
    /* No filled bar: the scrim baked into the backdrop already guarantees
       contrast, so the artwork stays visible all the way to the top edge. */
    const char *state = s->state == MEDIA_PAUSED ? "PAUSED"
                      : s->state == MEDIA_BUFFERING ? "BUFFERING"
                      : s->state == MEDIA_ERROR ? "ERROR"
                      : s->state == MEDIA_STOPPED ? "STOPPED" : "PLAYING";
    rg_color_t state_color = s->state == MEDIA_ERROR ? C_LIGHT_CORAL
                           : s->state == MEDIA_BUFFERING ? C_GOLD
                           : s->state == MEDIA_PLAYING ? MEDIA_RGB(0, 240, 120) : C_SILVER;

    int text_y = 4;
    int font_height = rg_gui_get_font_height();
    fill_rect(10, text_y + font_height / 2 - 3, 6, 6, state_color);
    rg_gui_draw_text(22, text_y, 130, state, state_color, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);

    char status[48];
    if (sleep_minutes)
        snprintf(status, sizeof(status), "Zz%um   %d%%", (unsigned)sleep_minutes, rg_audio_get_volume());
    else
        snprintf(status, sizeof(status), "%d%%", rg_audio_get_volume());
    rg_gui_draw_text(gui.width - 140, text_y, 130, status, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);

    /* Page dots, so eight pages stay navigable instead of feeling endless. */
    int dot = 5, spacing = 11;
    int total = PAGE_COUNT * spacing - (spacing - dot);
    int x = (gui.width - total) / 2;
    for (int i = 0; i < PAGE_COUNT; i++)
        fill_rect(x + i * spacing, text_y + font_height + 2, dot, dot,
                  i == (int)page ? C_WHITE : MEDIA_RGB(110, 120, 140));
    rg_gui_draw_text(0, text_y, gui.width, page_name(page), MEDIA_RGB(150, 210, 255), C_TRANSPARENT,
                     RG_TEXT_ALIGN_CENTER);
}

/* The hero block: large title over the artwork, the way a phone player does it. */
static void draw_hero(const media_player_snapshot_t *s, int scroll)
{
    int margin = RG_MAX(14, gui.width / 20);
    int width = gui.width - margin * 2;
    int y = meta_top;
    marquee_active = false;

    int row = rg_gui_get_font_height() + 3;
    marquee(margin, y, width, current_meta.title, C_WHITE, RG_TEXT_BIGGER, scroll);
    /* RG_TEXT_BIGGER doubles the glyph height. */
    y += row * 2 - 4;
    marquee(margin, y, width, current_meta.artist[0] ? current_meta.artist : "Unknown artist",
            MEDIA_RGB(140, 220, 255), 0, scroll);
    y += row;
    if (current_meta.album[0]) {
        marquee(margin, y, width, current_meta.album, MEDIA_RGB(170, 180, 195), 0, scroll);
        y += row;
    }

    char line[96];
    snprintf(line, sizeof(line), "%s  %lu kbps  %lu Hz  %s%s", media_format_name(s->format),
             (unsigned long)((s->bitrate ? s->bitrate : current_meta.bitrate) / 1000),
             (unsigned long)(s->sample_rate ? s->sample_rate : current_meta.sample_rate),
             s->channels == 1 ? "Mono" : "Stereo", media_eq_is_active() ? "  EQ" : "");
    rg_gui_draw_text(margin, y, width, line, MEDIA_RGB(130, 140, 155), C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

/* One compact line for the pages that need their vertical space. */
static void draw_compact_title(const media_player_snapshot_t *s, int scroll)
{
    char line[MEDIA_TEXT_MAX * 2 + 8];
    if (current_meta.artist[0])
        snprintf(line, sizeof(line), "%s  -  %s", current_meta.title, current_meta.artist);
    else
        snprintf(line, sizeof(line), "%s", current_meta.title);
    marquee_active = false;
    marquee(14, meta_top, gui.width - 28, line, C_WHITE, 0, scroll);
}

static void draw_progress(const media_player_snapshot_t *s)
{
    int margin = RG_MAX(14, gui.width / 20);
    int bar_width = gui.width - margin * 2;
    int bar_y = progress_top + rg_gui_get_font_height() + 6;
    int bar_height = 5;
    uint32_t duration = RG_MAX(1u, s->duration_ms);
    int buffered = (int)((uint64_t)bar_width * RG_MIN(s->buffered_ms, duration) / duration);
    int played = (int)((uint64_t)bar_width * RG_MIN(s->position_ms, duration) / duration);

    fill_rect(margin, bar_y, bar_width, bar_height, MEDIA_RGB(70, 78, 92));
    fill_rect(margin, bar_y, RG_MIN(bar_width, buffered), bar_height, MEDIA_RGB(120, 132, 150));
    fill_gradient(margin, bar_y, RG_MIN(bar_width, played), bar_height, MEDIA_RGB(80, 225, 255),
                  MEDIA_RGB(0, 160, 235));

    /* Playhead: a light pill with a dark surround, so it reads against both a
       bright and a dark patch of artwork. */
    int knob = margin + RG_MIN(bar_width - 1, played);
    fill_rect(knob - 4, bar_y - 5, 9, bar_height + 10, MEDIA_RGB(10, 14, 20));
    fill_rect(knob - 3, bar_y - 4, 7, bar_height + 8, C_WHITE);

    char elapsed[16], total[16], remaining[20];
    media_format_time(s->position_ms, elapsed, sizeof(elapsed));
    media_format_time(s->duration_ms, total, sizeof(total));
    media_format_time(s->duration_ms > s->position_ms ? s->duration_ms - s->position_ms : 0, remaining + 1,
                      sizeof(remaining) - 1);
    remaining[0] = '-';
    int third = bar_width / 3;
    rg_gui_draw_text(margin, progress_top + 1, third, elapsed, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(margin + third, progress_top + 1, third, total, MEDIA_RGB(170, 180, 195), C_TRANSPARENT,
                     RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(margin + third * 2, progress_top + 1, bar_width - third * 2, remaining, MEDIA_RGB(170, 180, 195),
                     C_TRANSPARENT, RG_TEXT_ALIGN_RIGHT);
}

static void draw_footer(page_t page)
{
    char footer[160];
    const char *hint;
    switch (page) {
    case PAGE_QUEUE: hint = "Up/Dn pick  A play"; break;
    case PAGE_EQUALIZER: hint = "L/R band  Up/Dn gain  A on/off  X preset"; break;
    case PAGE_SPECTRUM: hint = "X style  L/R track"; break;
    default: hint = "L/R track  Left/Right seek"; break;
    }
    char position[24] = "";
    if (playlist_count > 1)
        snprintf(position, sizeof(position), "%d/%d  ", current_track + 1, (int)playlist_count);
    snprintf(footer, sizeof(footer), "%s%s  |  %s", position, play_mode_name(), hint);

    fill_gradient(0, footer_top - 6, gui.width, 6, MEDIA_RGB(0, 0, 0), MEDIA_RGB(8, 10, 14));
    fill_rect(0, footer_top, gui.width, gui.height - footer_top, MEDIA_RGB(8, 10, 14));
    rg_gui_draw_text(0, footer_top + 2, gui.width, footer, MEDIA_RGB(150, 160, 178), C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

static void draw_osd(const char *label, int value)
{
    int width = gui.width * 2 / 3;
    int x = (gui.width - width) / 2;
    int y = content_bottom - 62;
    fill_rect(x, y, width, 44, MEDIA_RGB(12, 16, 22));
    fill_rect(x, y, width, 1, C_AQUA);
    fill_rect(x, y + 43, width, 1, C_AQUA);
    rg_gui_draw_text(x + 6, y + 6, width - 12, label, C_WHITE, C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
    int inner = width - 24;
    fill_rect(x + 12, y + 27, inner, 6, MEDIA_RGB(60, 68, 82));
    fill_rect(x + 12, y + 27, inner * RG_MIN(value, 100) / 100, 6, MEDIA_RGB(0, 200, 255));
}

static void osd_rect(int *x, int *y, int *w, int *h)
{
    *w = gui.width * 2 / 3;
    *x = (gui.width - *w) / 2;
    *y = content_bottom - 62;
    *h = 44;
}

/*******************************************************************************
 * Options menu
 *
 * Everything the button shortcuts can do is also reachable here, so the player
 * behaves like the rest of the launcher: MENU opens it.
 ******************************************************************************/

static rg_gui_event_t play_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) play_mode = (play_mode + PLAY_MODE_COUNT - 1) % PLAY_MODE_COUNT;
    if (event == RG_DIALOG_NEXT) play_mode = (play_mode + 1) % PLAY_MODE_COUNT;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        if (play_mode == PLAY_SHUFFLE) rebuild_shuffle(current_track);
        rg_settings_set_number(NS_APP, SETTING_MEDIA_PLAY_MODE, play_mode);
    }
    strcpy(option->value, play_mode_name());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t sleep_timer_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const uint16_t choices[] = {0, 15, 30, 45, 60, 90, 120};
    int index = 0;
    for (size_t i = 0; i < RG_COUNT(choices); i++)
        if (choices[i] == sleep_minutes) index = i;

    if (event == RG_DIALOG_PREV) index = (index + RG_COUNT(choices) - 1) % RG_COUNT(choices);
    if (event == RG_DIALOG_NEXT) index = (index + 1) % RG_COUNT(choices);
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        sleep_minutes = choices[index];
        sleep_deadline = sleep_minutes ? rg_system_timer() + (int64_t)sleep_minutes * 60 * 1000000 : 0;
        rg_settings_set_number(NS_APP, SETTING_MEDIA_SLEEP, sleep_minutes);
    }

    if (sleep_minutes) snprintf(option->value, 31, "%u min", (unsigned)sleep_minutes);
    else strcpy(option->value, "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t spectrum_style_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) spectrum_style = (spectrum_style + SPECTRUM_STYLE_COUNT - 1) % SPECTRUM_STYLE_COUNT;
    if (event == RG_DIALOG_NEXT) spectrum_style = (spectrum_style + 1) % SPECTRUM_STYLE_COUNT;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_settings_set_number(NS_APP, SETTING_MEDIA_SPECTRUM, spectrum_style);
    strcpy(option->value, spectrum_style_name(spectrum_style));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t volume_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) rg_audio_set_volume(RG_MAX(0, rg_audio_get_volume() - 5));
    if (event == RG_DIALOG_NEXT) rg_audio_set_volume(RG_MIN(100, rg_audio_get_volume() + 5));
    snprintf(option->value, 31, "%d%%", rg_audio_get_volume());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t brightness_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) rg_display_set_backlight(RG_MAX(1, rg_display_get_backlight() - 10));
    if (event == RG_DIALOG_NEXT) rg_display_set_backlight(RG_MIN(100, rg_display_get_backlight() + 10));
    snprintf(option->value, 31, "%d%%", (int)rg_display_get_backlight());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t eq_enable_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
        media_eq_set_enabled(!media_eq_get_enabled());
    strcpy(option->value, media_eq_get_enabled() ? "On" : "Off");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t eq_preset_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV)
        media_eq_set_preset((media_eq_get_preset() + MEDIA_EQ_PRESET_COUNT - 1) % MEDIA_EQ_PRESET_COUNT);
    if (event == RG_DIALOG_NEXT)
        media_eq_set_preset((media_eq_get_preset() + 1) % MEDIA_EQ_PRESET_COUNT);
    strcpy(option->value, media_eq_preset_name(media_eq_get_preset()));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t eq_band_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int band = (int)option->arg;
    if (event == RG_DIALOG_PREV) media_eq_set_gain(band, media_eq_get_gain(band) - 1);
    if (event == RG_DIALOG_NEXT) media_eq_set_gain(band, media_eq_get_gain(band) + 1);
    snprintf(option->value, 31, "%+d dB", media_eq_get_gain(band));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t eq_menu_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER) {
        snprintf(option->value, 31, "%s%s", media_eq_get_enabled() ? "" : "off, ",
                 media_eq_preset_name(media_eq_get_preset()));
        return RG_DIALOG_VOID;
    }

    /* Band labels have to outlive the dialog, so they are built once. */
    static char labels[MEDIA_EQ_BANDS][16];
    for (int i = 0; i < MEDIA_EQ_BANDS; i++) {
        uint16_t frequency = media_eq_frequencies[i];
        if (frequency >= 1000) snprintf(labels[i], sizeof(labels[i]), "%u kHz", frequency / 1000);
        else snprintf(labels[i], sizeof(labels[i]), "%u Hz", frequency);
    }

    const rg_gui_option_t options[] = {
        {0, "Equalizer", "-", RG_DIALOG_FLAG_NORMAL, &eq_enable_cb},
        {0, "Preset",    "-", RG_DIALOG_FLAG_NORMAL, &eq_preset_cb},
        RG_DIALOG_SEPARATOR,
        {0, labels[0], "-", RG_DIALOG_FLAG_NORMAL, &eq_band_cb},
        {1, labels[1], "-", RG_DIALOG_FLAG_NORMAL, &eq_band_cb},
        {2, labels[2], "-", RG_DIALOG_FLAG_NORMAL, &eq_band_cb},
        {3, labels[3], "-", RG_DIALOG_FLAG_NORMAL, &eq_band_cb},
        {4, labels[4], "-", RG_DIALOG_FLAG_NORMAL, &eq_band_cb},
        RG_DIALOG_END,
    };
    /* No commit here: the caller flushes once when the outer menu closes. */
    rg_gui_dialog("Equalizer", options, 0);
    return RG_DIALOG_REDRAW;
}

static rg_gui_event_t track_info_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    media_player_snapshot_t s;
    media_player_get_snapshot(&s);
    /* Static: this runs inside a dialog callback, inside a dialog, inside the
       player, and opens yet another dialog to display itself. */
    static char message[512];
    char duration[16], position[16];
    media_format_time(s.duration_ms, duration, sizeof(duration));
    media_format_time(s.position_ms, position, sizeof(position));
    snprintf(message, sizeof(message),
             "%.40s\n%.40s\n%.40s\n\n%s  %lu kbps  %lu Hz  %s\n%s / %s\n\nBuffer %u%%  Underruns %lu",
             current_meta.title[0] ? current_meta.title : "-",
             current_meta.artist[0] ? current_meta.artist : "Unknown artist",
             current_meta.album[0] ? current_meta.album : "-",
             media_format_name(s.format),
             (unsigned long)((s.bitrate ? s.bitrate : current_meta.bitrate) / 1000),
             (unsigned long)(s.sample_rate ? s.sample_rate : current_meta.sample_rate),
             s.channels == 1 ? "Mono" : "Stereo", position, duration,
             (unsigned)s.buffer_fill, (unsigned long)s.underruns);
    rg_gui_alert("Track info", message);
    return RG_DIALOG_REDRAW;
}

static rg_gui_event_t system_options_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) {
        rg_gui_options_menu();
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

/* Returns true when playback should stop (the user chose Stop). */
static bool media_options_menu(void)
{
    const rg_gui_option_t options[] = {
        {0, "Equalizer",     "-", RG_DIALOG_FLAG_NORMAL, &eq_menu_cb},
        {0, "Play mode",     "-", RG_DIALOG_FLAG_NORMAL, &play_mode_cb},
        {0, "Sleep timer",   "-", RG_DIALOG_FLAG_NORMAL, &sleep_timer_cb},
        {0, "Spectrum",      "-", RG_DIALOG_FLAG_NORMAL, &spectrum_style_cb},
        RG_DIALOG_SEPARATOR,
        {0, "Volume",        "-", RG_DIALOG_FLAG_NORMAL, &volume_cb},
        {0, "Brightness",    "-", RG_DIALOG_FLAG_NORMAL, &brightness_cb},
        RG_DIALOG_SEPARATOR,
        {0, "Track info",    NULL, RG_DIALOG_FLAG_NORMAL, &track_info_cb},
        {0, "More settings", NULL, RG_DIALOG_FLAG_NORMAL, &system_options_cb},
        RG_DIALOG_SEPARATOR,
        {1, "Stop playback", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };

    intptr_t choice = rg_gui_dialog("Music Player", options, 0);
    /* Several of the callbacks only set values; flush them all in one write. */
    save_equalizer();
    return choice == 1;
}

/*******************************************************************************
 * Player loop
 ******************************************************************************/

static bool page_is_hero(page_t page) { return page == PAGE_NOW_PLAYING; }
static bool page_is_animated(page_t page)
{
    return page == PAGE_SPECTRUM || page == PAGE_VU || page == PAGE_WAVEFORM;
}

/* Every band is sized from the active font rather than from fixed pixels, so
   changing the font does not silently make rows overlap. */
static void compute_layout(page_t page)
{
    int row = rg_gui_get_font_height() + 3;

    footer_top = gui.height - (row + 2);
    /* One row of times plus the bar and its playhead. */
    progress_top = footer_top - (row + 20);
    header_bottom = row + 12;

    content_top = header_bottom;
    if (page_is_hero(page)) {
        /* Double-height title, artist, album, then the format line. */
        meta_top = progress_top - (row * 5 + 4);
        content_bottom = meta_top;
    } else {
        meta_top = progress_top - row;
        content_bottom = meta_top - 2;
    }
}

static void show_player(void)
{
    page_t page = PAGE_NOW_PLAYING;
    int queue_selection = current_track > 0 ? current_track : 0;
    int eq_band = 0;
    uint32_t previous_keys = 0;
    int64_t next_draw = 0, repeat_at = 0, next_service = 0, marquee_at = 0;
    int marquee_step = 0;
    int32_t seek_step = 5000;
    bool analyzer = false;
    bool eq_dirty = false;

    /* Redraw bookkeeping. Everything defaults to "draw once" and is then only
       repainted when the thing it shows actually changed. */
    bool need_full = true, need_content = true, need_header = true, need_meta = true;
    bool need_progress = true, need_footer = true;
    char last_status[64] = "";
    char last_footer[64] = "";
    int last_played_px = -1, last_buffered_px = -1;
    uint32_t last_second = UINT32_MAX;
    int last_lyric = -2;
    int last_queue_selection = -1;
    int last_state = -1;
    bool queue_pending = false;
    int64_t osd_until = 0;
    bool osd_drawn = false;
    char osd_label[24] = "";
    int osd_value = 0;

    int old_font = rg_gui_get_font();
    rg_gui_set_font(RG_GUI_FONT_MEDIA);
    compute_layout(page);
    rg_input_wait_for_key(RG_KEY_ALL, false, 500);

    while (true) {
        uint32_t keys = rg_input_read_gamepad();
        uint32_t pressed = keys & ~previous_keys;
        bool option = (keys & RG_KEY_OPTION) != 0;
        media_player_snapshot_t s;
        media_player_get_snapshot(&s);

        if (pressed & RG_KEY_B)
            break;

        if (pressed & RG_KEY_MENU) {
            /* The dialog draws straight to the panel, so hand the screen back
               first and repaint everything once it closes. */
            rg_gui_set_surface(NULL);
            bool stop = media_options_menu();
            if (stop) {
                save_resume(true);
                media_player_stop();
            }
            compute_layout(page);
            rg_input_wait_for_key(RG_KEY_ALL, false, 300);
            need_full = true;
            previous_keys = rg_input_read_gamepad();
            continue;
        }

        if (pressed & RG_KEY_A) {
            if (page == PAGE_EQUALIZER) {
                media_eq_set_enabled(!media_eq_get_enabled());
                eq_dirty = true;
                need_content = need_meta = true;
            } else if (page == PAGE_QUEUE) {
                if (play_playlist_track(queue_selection)) need_full = true;
            } else if (s.state == MEDIA_STOPPED || s.state == MEDIA_ERROR) {
                if (play_playlist_track(current_track)) need_full = true;
            } else {
                media_player_toggle_pause();
            }
        }

        if (pressed & RG_KEY_START) {
            if (option) {
                cycle_sleep_timer();
                need_header = need_footer = true;
            } else {
                save_resume(true);
                media_player_stop();
            }
        }

        if (pressed & RG_KEY_SELECT) {
            if (option) {
                cycle_play_mode();
                need_footer = true;
            } else {
                page = (page + 1) % PAGE_COUNT;
                if (page == PAGE_QUEUE) queue_selection = current_track > 0 ? current_track : 0;
                compute_layout(page);
                need_full = true;
            }
        }

        if (pressed & RG_KEY_L) { int i = adjacent_track(current_track, -1, false); if (i >= 0 && play_playlist_track(i)) need_full = true; }
        if (pressed & RG_KEY_R) { int i = adjacent_track(current_track, 1, false); if (i >= 0 && play_playlist_track(i)) need_full = true; }

        if (pressed & (RG_KEY_UP | RG_KEY_DOWN)) {
            int direction = (pressed & RG_KEY_UP) ? 1 : -1;
            if (page == PAGE_EQUALIZER && !option) {
                media_eq_set_gain(eq_band, media_eq_get_gain(eq_band) + direction);
                eq_dirty = true;
                need_content = need_meta = true;
            } else if (page == PAGE_QUEUE && !option) {
                if (playlist_count) {
                    queue_selection = (queue_selection - direction + (int)playlist_count) % (int)playlist_count;
                    need_content = true;
                }
            } else if (option) {
                rg_display_set_backlight(RG_MIN(100, RG_MAX(1, rg_display_get_backlight() + direction * 10)));
                snprintf(osd_label, sizeof(osd_label), "Brightness");
                osd_value = rg_display_get_backlight();
                osd_until = rg_system_timer() + 1200000;
            } else {
                rg_audio_set_volume(RG_MIN(100, RG_MAX(0, rg_audio_get_volume() + direction * 5)));
                snprintf(osd_label, sizeof(osd_label), "Volume");
                osd_value = rg_audio_get_volume();
                osd_until = rg_system_timer() + 1200000;
            }
        }

        if (pressed & (RG_KEY_LEFT | RG_KEY_RIGHT)) {
            int direction = (pressed & RG_KEY_RIGHT) ? 1 : -1;
            if (page == PAGE_EQUALIZER && !option) {
                eq_band = (eq_band + direction + MEDIA_EQ_BANDS) % MEDIA_EQ_BANDS;
                need_content = true;
            } else if (option || !s.seekable) {
                int i = adjacent_track(current_track, direction, false);
                if (i >= 0 && play_playlist_track(i)) need_full = true;
            } else {
                seek_step = 5000;
                media_player_seek(direction * seek_step);
                need_progress = true;
            }
        }

        if (pressed & RG_KEY_X) {
            if (page == PAGE_EQUALIZER) {
                media_eq_set_preset((media_eq_get_preset() + 1) % MEDIA_EQ_PRESET_COUNT);
                eq_dirty = true;
                need_content = need_meta = true;
            } else if (page == PAGE_SPECTRUM) {
                spectrum_style = (spectrum_style + 1) % SPECTRUM_STYLE_COUNT;
                rg_settings_set_number(NS_APP, SETTING_MEDIA_SPECTRUM, spectrum_style);
                need_content = true;
            } else {
                rg_display_set_backlight(RG_MAX(1, rg_display_get_backlight() - 10));
                snprintf(osd_label, sizeof(osd_label), "Brightness");
                osd_value = rg_display_get_backlight();
                osd_until = rg_system_timer() + 1200000;
            }
        }
        if (pressed & RG_KEY_Y) {
            rg_display_set_backlight(RG_MIN(100, rg_display_get_backlight() + 10));
            snprintf(osd_label, sizeof(osd_label), "Brightness");
            osd_value = rg_display_get_backlight();
            osd_until = rg_system_timer() + 1200000;
        }

        int64_t now = rg_system_timer();
        bool seek_held = !option && s.seekable && page != PAGE_EQUALIZER && (keys & (RG_KEY_LEFT | RG_KEY_RIGHT));
        if (keys != previous_keys) {
            repeat_at = now + 450000;
            if (!(keys & (RG_KEY_LEFT | RG_KEY_RIGHT))) seek_step = 5000;
        } else if (seek_held && now >= repeat_at) {
            /* Accelerate while held: short taps stay precise, long holds cross
               a long track quickly. */
            seek_step = RG_MIN(60000, seek_step + 5000);
            media_player_seek((keys & RG_KEY_LEFT) ? -seek_step : seek_step);
            repeat_at = now + 160000;
            need_progress = true;
        }

        if (play_finished_track())
            need_full = true;
        if (now >= next_service) {
            playback_service();
            next_service = now + 100000;
        }
        /* Artwork touches the SD card, so wait until the stream is stable. */
        if (artwork_pending && (s.state == MEDIA_PLAYING || s.state == MEDIA_ERROR || s.state == MEDIA_STOPPED)) {
            refresh_artwork();
            need_full = true;
        }

        bool animated = page_is_animated(page);
        if (animated != analyzer) {
            media_player_set_analyzer(animated);
            analyzer = animated;
        }

        /* A visualiser frame pushes the whole content band over SPI, which is
           about 21 ms of DMA at 80 MHz on a 480x320 panel. 25 fps keeps the
           bus around half busy and leaves the UI responsive; the eye cannot
           tell the difference on a bar graph. */
        int64_t interval = animated ? 40000 : 100000;
        if (now < next_draw && !need_full && !need_content && !pressed) {
            /* One FreeRTOS tick. Anything shorter rounds down to zero at the
               configured 100 Hz tick rate and turns this into a busy loop. */
            rg_task_delay(10);
            previous_keys = keys;
            continue;
        }
        next_draw = now + interval;

        /* The snapshot taken before the key handling may describe the track we
           just skipped away from, so refresh it before deciding what to draw. */
        media_player_get_snapshot(&s);

        char status[64];
        snprintf(status, sizeof(status), "%d|%u|%d|%d", (int)s.state, (unsigned)sleep_minutes,
                 rg_audio_get_volume(), (int)page);
        if (strcmp(status, last_status)) { snprintf(last_status, sizeof(last_status), "%s", status); need_header = true; }

        char footer[64];
        snprintf(footer, sizeof(footer), "%d|%d|%d", (int)play_mode, (int)page, current_track);
        if (strcmp(footer, last_footer)) { snprintf(last_footer, sizeof(last_footer), "%s", footer); need_footer = true; }

        uint32_t duration = RG_MAX(1u, s.duration_ms);
        int bar_width = gui.width - RG_MAX(14, gui.width / 20) * 2;
        int played_px = (int)((uint64_t)bar_width * RG_MIN(s.position_ms, duration) / duration);
        int buffered_px = (int)((uint64_t)bar_width * RG_MIN(s.buffered_ms, duration) / duration);
        if (played_px != last_played_px || buffered_px != last_buffered_px || s.position_ms / 1000 != last_second) {
            last_played_px = played_px;
            last_buffered_px = buffered_px;
            last_second = s.position_ms / 1000;
            need_progress = true;
        }

        /* The error banner lives in the content region, so any state change has
           to repaint it (or clear it). */
        if ((int)s.state != last_state) {
            last_state = (int)s.state;
            need_content = true;
        }

        if (animated) {
            if (analyze_audio(s.state == MEDIA_PLAYING))
                need_content = true;
        } else if (page == PAGE_LYRICS) {
            int active = media_lyrics_find(&player_lyrics, s.position_ms);
            if (active != last_lyric) { last_lyric = active; need_content = true; }
        } else if (page == PAGE_QUEUE) {
            if (queue_selection != last_queue_selection || queue_pending) {
                last_queue_selection = queue_selection;
                need_content = true;
            }
        } else if (page == PAGE_DETAILS && need_progress) {
            need_content = true;
        }
        if (marquee_active && now >= marquee_at) {
            marquee_at = now + 220000;
            marquee_step++;
            need_meta = true;
        }

        bool osd_active = now < osd_until;
        if (osd_active != osd_drawn) {
            osd_drawn = osd_active;
            need_content = true;
        }

        if (!need_full && !need_content && !need_header && !need_meta && !need_progress && !need_footer && !osd_active) {
            rg_task_delay(10);
            previous_keys = keys;
            continue;
        }

        rg_gui_set_surface(gui.surface);

        if (need_full) {
            restore_rect(0, 0, gui.width, gui.height);
            mark_dirty(0, 0, gui.width, gui.height);
            need_content = need_header = need_meta = need_progress = need_footer = true;
        }
        if (need_content) {
            if (!need_full)
                restore_rect(0, content_top, gui.width, content_bottom - content_top);
            mark_dirty(0, content_top, gui.width, content_bottom - content_top);
            switch (page) {
            case PAGE_SPECTRUM: draw_spectrum(); break;
            case PAGE_VU: draw_vu(); break;
            case PAGE_WAVEFORM: draw_waveform(); break;
            case PAGE_LYRICS: draw_lyrics(&s); break;
            case PAGE_EQUALIZER: draw_equalizer(eq_band); break;
            case PAGE_QUEUE: queue_pending = draw_queue(queue_selection); break;
            case PAGE_DETAILS: draw_details(&s); break;
            default: break; /* the hero page has no content region of its own */
            }
            if (s.state == MEDIA_ERROR && s.error[0]) {
                rg_gui_draw_text(10, (content_top + content_bottom) / 2, gui.width - 20, s.error, C_LIGHT_CORAL,
                                 C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
            }
        }
        if (need_meta) {
            if (!need_full)
                restore_rect(0, meta_top - 2, gui.width, progress_top - meta_top + 2);
            mark_dirty(0, meta_top - 2, gui.width, progress_top - meta_top + 2);
            if (page_is_hero(page))
                draw_hero(&s, marquee_step);
            else
                draw_compact_title(&s, marquee_step);
        }
        if (osd_active) {
            int x, y, w, h;
            osd_rect(&x, &y, &w, &h);
            if (!need_content)
                restore_rect(x, y, w, h);
            mark_dirty(x, y, w, h);
            draw_osd(osd_label, osd_value);
        }
        if (need_header) {
            if (!need_full)
                restore_rect(0, 0, gui.width, header_bottom);
            mark_dirty(0, 0, gui.width, header_bottom);
            draw_header(&s, page);
        }
        if (need_progress) {
            restore_rect(0, progress_top, gui.width, footer_top - progress_top);
            mark_dirty(0, progress_top, gui.width, footer_top - progress_top);
            draw_progress(&s);
        }
        if (need_footer) {
            mark_dirty(0, footer_top - 6, gui.width, gui.height - footer_top + 6);
            draw_footer(page);
        }

        rg_gui_set_surface(NULL);
        flush_dirty();

        need_full = need_content = need_header = need_meta = need_progress = need_footer = false;
        previous_keys = keys;
        rg_task_delay(10);
    }

    if (analyzer)
        media_player_set_analyzer(false);
    if (eq_dirty)
        save_equalizer();
    save_resume(true);
    rg_gui_set_font(old_font);
    rg_input_wait_for_key(RG_KEY_ALL, false, 300);
    gui_redraw();
}

/*******************************************************************************
 * Browser integration
 ******************************************************************************/

static void update_browser_preview(tab_t *tab, media_entry_t *entry)
{
    gui_set_preview(tab, NULL);
    if (!entry) return;
    char path[RG_PATH_MAX + 1], status[32] = "";
    path_for_entry(entry, path, sizeof(path));

    if (entry->type == ENTRY_MEDIA) {
        if (!entry->metadata_loaded) {
            entry->metadata_loaded = media_metadata_read(path, &entry->metadata, true);
            listbox_item_t *item = gui_get_selected_item(tab);
            if (item && entry->metadata_loaded)
                format_entry_text(item, entry);
        }
        gui_set_preview(tab, media_metadata_load_cover(path, &entry->metadata, gui.width / 2, gui.height * 2 / 3));
        char duration[16];
        media_format_time(entry->metadata.duration_ms, duration, sizeof(duration));
        snprintf(status, sizeof(status), "%s %s %luk", media_format_name(entry->metadata.format), duration,
                 (unsigned long)(entry->metadata.bitrate / 1000));
    } else if (entry->type == ENTRY_DIRECTORY) {
        char probe[RG_PATH_MAX + 1];
        size_t length = strlen(path);
        if (length + 2 < sizeof(probe)) {
            memcpy(probe, path, length);
            memcpy(probe + length, "/_", 3);
            memset(&scratch_meta, 0, sizeof(scratch_meta));
            gui_set_preview(tab, media_metadata_load_cover(probe, &scratch_meta, gui.width / 2, gui.height * 2 / 3));
        }
        snprintf(status, sizeof(status), "Album folder");
    } else {
        snprintf(status, sizeof(status), "M3U playlist");
    }
    gui_set_status(tab, NULL, status);
}

/* While browsing, show what is playing so the player is not a dead end. */
static void update_browser_status(tab_t *tab)
{
    media_player_snapshot_t s;
    media_player_get_snapshot(&s);
    if (!media_ready || s.state == MEDIA_STOPPED) {
        gui_set_status(tab, "", NULL);
        return;
    }
    char line[64], position[16];
    media_format_time(s.position_ms, position, sizeof(position));
    snprintf(line, sizeof(line), "%s %s %.30s", s.state == MEDIA_PAUSED ? "||" : ">", position, current_meta.title);
    gui_set_status(tab, line, NULL);
}

static void event_handler(gui_event_t event, tab_t *tab)
{
    listbox_item_t *item = gui_get_selected_item(tab);
    media_entry_t *entry = item ? item->arg : NULL;

    if (event == TAB_INIT) {
        scan_directory(tab, current_path, NULL);
    } else if (event == TAB_ENTER || event == TAB_SCROLL) {
        if (scanning_directory) {
            preview_pending = true;
        } else {
            update_browser_preview(tab, entry);
            update_browser_status(tab);
        }
    } else if (event == TAB_IDLE) {
        update_browser_status(tab);
    } else if (event == TAB_LEAVE) {
        gui_set_preview(tab, NULL);
    } else if (event == TAB_ACTION && entry) {
        char path[RG_PATH_MAX + 1];
        path_for_entry(entry, path, sizeof(path));
        if (entry->type == ENTRY_DIRECTORY) {
            scan_directory(tab, path, NULL);
        } else if (entry->type == ENTRY_PLAYLIST ? load_m3u(path) : play_track(track_index(entry))) {
            /* Cover art is deferred, so start on the gradient and let the
               player swap in the artwork once the stream is stable. */
            build_backdrop();
            show_player();
        }
    } else if (event == TAB_BACK) {
        if (!strcmp(current_path, RG_BASE_PATH_MEDIA)) {
            tab->navpath = NULL;
        } else {
            char selected[RG_PATH_MAX + 1];
            snprintf(selected, sizeof(selected), "%s", rg_basename(current_path));
            char parent[RG_PATH_MAX + 1];
            snprintf(parent, sizeof(parent), "%s", current_path);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = 0;
            scan_directory(tab, parent, selected);
        }
    }
}

void media_library_tick(void)
{
    static int64_t next_tick;
    int64_t now = rg_system_timer();
    if (now < next_tick) return;
    next_tick = now + 20000;
    playback_service();
    play_finished_track();

    /* Top of the main loop: the safe place to do the deep, allocation-heavy
       preview work that scan_directory() deferred. */
    if (preview_pending && media_tab && gui_get_current_tab() == media_tab) {
        preview_pending = false;
        listbox_item_t *item = gui_get_selected_item(media_tab);
        update_browser_preview(media_tab, item ? item->arg : NULL);
        update_browser_status(media_tab);
        gui_redraw();
    }
    /* Nothing is drawing the player right now, so this is a safe moment to
       finish the deferred SD work. */
    if (artwork_pending) {
        media_player_snapshot_t s;
        media_player_get_snapshot(&s);
        if (s.state == MEDIA_PLAYING || s.state == MEDIA_STOPPED || s.state == MEDIA_ERROR)
            refresh_artwork();
    }
}

void media_library_init(void)
{
    snprintf(current_path, sizeof(current_path), "%s", RG_BASE_PATH_MEDIA);
    if (!rg_storage_exists(current_path)) rg_storage_mkdir(current_path);

    resume_path = rg_settings_get_string(NS_APP, SETTING_MEDIA_PATH, NULL);
    resume_position = rg_settings_get_number(NS_APP, SETTING_MEDIA_POSITION, 0);
    int saved_mode = rg_settings_get_number(NS_APP, SETTING_MEDIA_PLAY_MODE, PLAY_REPEAT_ALL);
    play_mode = saved_mode >= 0 && saved_mode < PLAY_MODE_COUNT ? saved_mode : PLAY_REPEAT_ALL;
    sleep_minutes = rg_settings_get_number(NS_APP, SETTING_MEDIA_SLEEP, 0);
    if (sleep_minutes)
        sleep_deadline = rg_system_timer() + (int64_t)sleep_minutes * 60 * 1000000;
    load_equalizer();
    int style = (int)rg_settings_get_number(NS_APP, SETTING_MEDIA_SPECTRUM, SPECTRUM_STYLE_BARS);
    spectrum_style = style >= 0 && style < SPECTRUM_STYLE_COUNT ? style : SPECTRUM_STYLE_BARS;
    /* Without a seed every boot produced the exact same "random" order. */
#ifdef ESP_PLATFORM
    srand(esp_random());
#else
    srand((unsigned)rg_system_timer());
#endif

#if defined(ESP_PLATFORM) && defined(RG_GPIO_HEADPHONE_DETECT)
    gpio_config_t detect_config = {
        .pin_bit_mask = 1ULL << RG_GPIO_HEADPHONE_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = RG_HEADPHONE_DETECT_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&detect_config);
    headphone_present = headphone_raw = gpio_get_level(RG_GPIO_HEADPHONE_DETECT) == RG_HEADPHONE_DETECT_LEVEL;
    headphone_changed_at = rg_system_timer();
#endif

    media_tab = gui_add_tab("music", "Music Player", NULL, event_handler);
    if (!media_tab) RG_LOGE("Music tab could not be registered");
    else gui_set_status(media_tab, "", "");
}
