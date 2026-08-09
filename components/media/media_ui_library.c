#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_playlist.h"
#include "media_ui_internal.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_UI"

#define NAV_DEPTH 8

typedef struct
{
    media_browse_mode_t mode;
    uint32_t filter_hash;
    char name[MEDIA_TAG_ALBUM_LEN];
    char folder[MEDIA_MAX_PATH + 1];
    int cursor;
    int scroll;
} nav_entry_t;

static nav_entry_t nav_stack[NAV_DEPTH];
static int nav_depth;

/* Categories shown on the browser home screen, in the order most players use. */
static const struct
{
    const char *label;
    media_browse_mode_t mode;
} home_entries[] = {
    {"Folders",         MEDIA_BROWSE_FOLDERS},
    {"Albums",          MEDIA_BROWSE_ALBUMS},
    {"Artists",         MEDIA_BROWSE_ARTISTS},
    {"Genres",          MEDIA_BROWSE_GENRES},
    {"Playlists",       MEDIA_BROWSE_PLAYLISTS},
    {"Favorites",       MEDIA_BROWSE_FAVORITES},
    {"Recently Played", MEDIA_BROWSE_RECENT},
    {"All Tracks",      MEDIA_BROWSE_ALL_TRACKS},
};

/* -------------------------------------------------------------------------------------- */
/* Folder listing                                                                           */
/* -------------------------------------------------------------------------------------- */

typedef struct
{
    media_list_t *list;
    int folders;
} folder_scan_t;

static int folder_scan_cb(const rg_scandir_t *file, void *arg)
{
    folder_scan_t *state = arg;

    if (media_path_is_hidden(file->basename))
        return RG_SCANDIR_CONTINUE;

    bool audio = file->is_file && media_path_is_audio(file->basename);
    bool playlist = file->is_file && media_path_is_playlist(file->basename);

    if (!file->is_dir && !audio && !playlist)
        return RG_SCANDIR_CONTINUE;

    media_list_item_t *item = media_list_add(state->list);
    if (!item)
        return RG_SCANDIR_STOP;

    if (file->is_dir)
    {
        snprintf(item->text, sizeof(item->text), "%s", file->basename);
        item->kind = 1;
        state->folders++;
    }
    else
    {
        media_path_stem(item->text, sizeof(item->text), file->basename);
        item->kind = playlist ? 4 : 2;
    }

    // The full path is needed for playback, so keep it in the subtext slot.
    media_utf8_copy(item->subtext, sizeof(item->subtext), file->basename);
    item->arg = 0;

    return RG_SCANDIR_CONTINUE;
}

static int compare_items(const void *a, const void *b)
{
    const media_list_item_t *ia = a, *ib = b;
    // Folders first, then natural order so "2 - Song" precedes "10 - Song".
    if (ia->kind != ib->kind)
        return (ia->kind == 1) ? -1 : ((ib->kind == 1) ? 1 : 0);
    return media_strnatcasecmp(ia->text, ib->text);
}

/** Build the absolute path for a folder-view row. */
static bool folder_item_path(const media_list_item_t *item, char *out, size_t out_size)
{
    return media_path_join(out, out_size, mui.folder, item->subtext);
}

/* -------------------------------------------------------------------------------------- */
/* List building                                                                            */
/* -------------------------------------------------------------------------------------- */

static void add_track_rows(const uint32_t *indices, uint32_t count, bool show_number)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const media_entry_t *entry = media_library_entry(indices[i]);
        if (!entry)
            continue;

        media_list_item_t *item = media_list_add(&mui.list);
        if (!item)
            break;

        const char *title = media_library_entry_title(entry);
        if (show_number && entry->track_number)
            snprintf(item->text, sizeof(item->text), "%2u.  %s", entry->track_number, title);
        else
            media_utf8_copy(item->text, sizeof(item->text), title);

        char duration[12];
        media_format_time(duration, sizeof(duration), (uint32_t)entry->duration_s * 1000);
        const char *artist = media_library_entry_artist(entry);
        snprintf(item->subtext, sizeof(item->subtext), "%s", duration);

        if (!show_number && artist[0])
            snprintf(item->subtext, sizeof(item->subtext), "%.20s  %s", artist, duration);

        item->kind = 2;
        item->arg = entry->id;
        item->flags = entry->flags;
    }
}

static void add_group_rows(media_view_t view)
{
    uint32_t count = media_library_group_count(view);

    for (uint32_t i = 0; i < count; ++i)
    {
        const media_group_t *group = media_library_group(view, i);
        if (!group)
            continue;

        media_list_item_t *item = media_list_add(&mui.list);
        if (!item)
            break;

        media_utf8_copy(item->text, sizeof(item->text), group->name);

        if (view == MEDIA_VIEW_ALBUMS)
        {
            if (group->year)
                snprintf(item->subtext, sizeof(item->subtext), "%.18s  %u  %ut", group->artist,
                         group->year, group->track_count);
            else
                snprintf(item->subtext, sizeof(item->subtext), "%.22s  %ut", group->artist,
                         group->track_count);
        }
        else
        {
            snprintf(item->subtext, sizeof(item->subtext), "%u tracks", group->track_count);
        }

        item->kind = 3;
        item->arg = group->hash;
    }
}

void media_ui_library_refresh(void)
{
    media_list_reset(&mui.list);

    switch (mui.browse)
    {
    case MEDIA_BROWSE_HOME:
        for (size_t i = 0; i < RG_COUNT(home_entries); ++i)
        {
            media_list_item_t *item = media_list_add(&mui.list);
            if (!item)
                break;
            media_utf8_copy(item->text, sizeof(item->text), home_entries[i].label);
            item->kind = 0;
            item->arg = (uint32_t)home_entries[i].mode;

            switch (home_entries[i].mode)
            {
            case MEDIA_BROWSE_ALBUMS:
                snprintf(item->subtext, sizeof(item->subtext), "%u",
                         (unsigned)media_library_group_count(MEDIA_VIEW_ALBUMS));
                break;
            case MEDIA_BROWSE_ARTISTS:
                snprintf(item->subtext, sizeof(item->subtext), "%u",
                         (unsigned)media_library_group_count(MEDIA_VIEW_ARTISTS));
                break;
            case MEDIA_BROWSE_GENRES:
                snprintf(item->subtext, sizeof(item->subtext), "%u",
                         (unsigned)media_library_group_count(MEDIA_VIEW_GENRES));
                break;
            case MEDIA_BROWSE_ALL_TRACKS:
                snprintf(item->subtext, sizeof(item->subtext), "%u",
                         (unsigned)media_library_track_count());
                break;
            default:
                break;
            }
        }
        break;

    case MEDIA_BROWSE_FOLDERS:
    {
        folder_scan_t state = {&mui.list, 0};
        rg_storage_scandir(mui.folder, folder_scan_cb, &state,
                           RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_SORT);
        if (mui.list.count > 1)
            qsort(mui.list.items, (size_t)mui.list.count, sizeof(media_list_item_t), compare_items);
        break;
    }

    case MEDIA_BROWSE_ALBUMS:
        add_group_rows(MEDIA_VIEW_ALBUMS);
        break;

    case MEDIA_BROWSE_ARTISTS:
        add_group_rows(MEDIA_VIEW_ARTISTS);
        break;

    case MEDIA_BROWSE_GENRES:
        add_group_rows(MEDIA_VIEW_GENRES);
        break;

    case MEDIA_BROWSE_ALBUM_TRACKS:
    case MEDIA_BROWSE_ARTIST_TRACKS:
    case MEDIA_BROWSE_GENRE_TRACKS:
    {
        media_view_t view = mui.browse == MEDIA_BROWSE_ALBUM_TRACKS   ? MEDIA_VIEW_ALBUMS
                            : mui.browse == MEDIA_BROWSE_ARTIST_TRACKS ? MEDIA_VIEW_ARTISTS
                                                                       : MEDIA_VIEW_GENRES;
        const uint32_t *indices = NULL;
        uint32_t count = media_library_query(view, mui.filter_hash, &indices);
        if (indices)
            add_track_rows(indices, count, view == MEDIA_VIEW_ALBUMS);
        break;
    }

    case MEDIA_BROWSE_FAVORITES:
    case MEDIA_BROWSE_RECENT:
    case MEDIA_BROWSE_ALL_TRACKS:
    {
        media_view_t view = mui.browse == MEDIA_BROWSE_FAVORITES ? MEDIA_VIEW_FAVORITES
                            : mui.browse == MEDIA_BROWSE_RECENT   ? MEDIA_VIEW_RECENT
                                                                  : MEDIA_VIEW_ALL_TRACKS;
        const uint32_t *indices = NULL;
        uint32_t count = media_library_query(view, 0, &indices);
        if (indices)
            add_track_rows(indices, count, false);
        break;
    }

    case MEDIA_BROWSE_PLAYLISTS:
    {
        // Only the names are kept in the rows; the paths are re-resolved on selection, which
        // avoids holding 32 full paths resident for a view the user may never act on.
        media_playlist_info_t *found = calloc(MEDIA_UI_MAX_PLAYLISTS, sizeof(media_playlist_info_t));
        if (found)
        {
            int count = media_playlist_list(media_library_root(), found, MEDIA_UI_MAX_PLAYLISTS);
            for (int i = 0; i < count; ++i)
            {
                media_list_item_t *item = media_list_add(&mui.list);
                if (!item)
                    break;
                media_utf8_copy(item->text, sizeof(item->text), found[i].name);
                media_utf8_copy(item->subtext, sizeof(item->subtext), "playlist");
                item->kind = 4;
                item->arg = (uint32_t)i;
            }
            free(found);
        }
        break;
    }

    case MEDIA_BROWSE_PLAYLIST_TRACKS:
    {
        media_playlist_t *pl = media_playlist_load(mui.playlist_path, media_library_root());
        if (pl)
        {
            for (int i = 0; i < pl->count; ++i)
            {
                const char *path = media_playlist_entry(pl, i);
                media_list_item_t *item = media_list_add(&mui.list);
                if (!item)
                    break;
                media_path_stem(item->text, sizeof(item->text), path);
                media_utf8_copy(item->subtext, sizeof(item->subtext), rg_basename(rg_dirname(path)));
                item->kind = 2;
                item->arg = media_library_find_by_path(path);
            }
            media_playlist_free(pl);
        }
        break;
    }

    default:
        break;
    }

    mui.list.cursor = media_clampi(mui.list.cursor, 0, RG_MAX(mui.list.count - 1, 0));
    media_list_move(&mui.list, 0, 0);
}

/* -------------------------------------------------------------------------------------- */
/* Navigation                                                                               */
/* -------------------------------------------------------------------------------------- */

void media_ui_library_enter(media_browse_mode_t mode, uint32_t filter_hash, const char *name)
{
    if (mode != MEDIA_BROWSE_HOME && nav_depth < NAV_DEPTH)
    {
        nav_entry_t *entry = &nav_stack[nav_depth++];
        entry->mode = mui.browse;
        entry->filter_hash = mui.filter_hash;
        media_utf8_copy(entry->name, sizeof(entry->name), mui.filter_name);
        media_utf8_copy(entry->folder, sizeof(entry->folder), mui.folder);
        entry->cursor = mui.list.cursor;
        entry->scroll = mui.list.scroll;
    }
    else if (mode == MEDIA_BROWSE_HOME)
    {
        nav_depth = 0;
    }

    mui.browse = mode;
    mui.filter_hash = filter_hash;
    media_utf8_copy(mui.filter_name, sizeof(mui.filter_name), name ?: "");

    if (mode == MEDIA_BROWSE_FOLDERS && !mui.folder[0])
        media_utf8_copy(mui.folder, sizeof(mui.folder), media_library_root());

    mui.list.cursor = 0;
    mui.list.scroll = 0;
    media_ui_library_refresh();
}

/** Returns false when we are already at the top and the player should exit. */
bool media_ui_library_back(void)
{
    // Inside the folder view, back walks up the tree before it pops the navigation stack.
    if (mui.browse == MEDIA_BROWSE_FOLDERS)
    {
        const char *root = media_library_root();
        if (strcmp(mui.folder, root) != 0)
        {
            char previous[MEDIA_TAG_ALBUM_LEN];
            media_utf8_copy(previous, sizeof(previous), rg_basename(mui.folder));
            media_utf8_copy(mui.folder, sizeof(mui.folder), rg_dirname(mui.folder));
            mui.list.cursor = 0;
            media_ui_library_refresh();

            // Land the cursor back on the folder we just left.
            for (int i = 0; i < mui.list.count; ++i)
            {
                if (mui.list.items[i].kind == 1 && strcmp(mui.list.items[i].text, previous) == 0)
                {
                    mui.list.cursor = i;
                    media_list_move(&mui.list, 0, 0);
                    break;
                }
            }
            return true;
        }
    }

    if (nav_depth <= 0)
        return false;

    nav_entry_t *entry = &nav_stack[--nav_depth];
    mui.browse = entry->mode;
    mui.filter_hash = entry->filter_hash;
    media_utf8_copy(mui.filter_name, sizeof(mui.filter_name), entry->name);
    media_utf8_copy(mui.folder, sizeof(mui.folder), entry->folder);

    media_ui_library_refresh();
    mui.list.cursor = media_clampi(entry->cursor, 0, RG_MAX(mui.list.count - 1, 0));
    mui.list.scroll = entry->scroll;
    media_list_move(&mui.list, 0, 0);
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* Playback from the browser                                                                */
/* -------------------------------------------------------------------------------------- */

/** Queue every track in the current list and start at `start`. */
static void play_list_from(int start)
{
    media_queue_clear();

    // Playlist rows are resolved against the playlist file; load it once rather than once
    // per row, which would be quadratic in file reads.
    media_playlist_t *playlist = NULL;
    if (mui.browse == MEDIA_BROWSE_PLAYLIST_TRACKS)
        playlist = media_playlist_load(mui.playlist_path, media_library_root());

    int queue_index = 0;
    int target = 0;

    for (int i = 0; i < mui.list.count; ++i)
    {
        const media_list_item_t *item = &mui.list.items[i];
        if (item->kind != 2)
            continue;

        char path[MEDIA_MAX_PATH + 1];
        const char *source = NULL;

        if (mui.browse == MEDIA_BROWSE_FOLDERS)
        {
            if (folder_item_path(item, path, sizeof(path)))
                source = path;
        }
        else if (item->arg)
        {
            media_track_t track;
            if (media_library_get_track(item->arg, &track))
            {
                media_utf8_copy(path, sizeof(path), track.path);
                source = path;
            }
        }
        else if (playlist)
        {
            const char *entry = media_playlist_entry(playlist, i);
            if (entry)
            {
                media_utf8_copy(path, sizeof(path), entry);
                source = path;
            }
        }

        if (!source)
            continue;

        if (i == start)
            target = queue_index;

        if (media_queue_add(source, item->arg))
            queue_index++;
    }

    media_playlist_free(playlist);

    if (media_queue_count() == 0)
        return;

    if (media_settings()->shuffle)
        media_queue_reshuffle();

    media_player_play_index(target);
    mui.page = media_settings()->default_page == MEDIA_PAGE_LIBRARY ? MEDIA_PAGE_NOW_PLAYING
                                                                    : media_settings()->default_page;
    mui.in_library = false;
}

/* -------------------------------------------------------------------------------------- */
/* Input                                                                                    */
/* -------------------------------------------------------------------------------------- */

bool media_ui_library_input(uint32_t key, bool repeat)
{
    media_list_item_t *item =
        (mui.list.cursor >= 0 && mui.list.cursor < mui.list.count) ? &mui.list.items[mui.list.cursor]
                                                                   : NULL;

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
        if (!item)
            return true;

        if (repeat)
        {
            // Holding A opens the context menu, matching the launcher's own long-press idiom.
            char path[MEDIA_MAX_PATH + 1] = {0};
            if (mui.browse == MEDIA_BROWSE_FOLDERS)
                folder_item_path(item, path, sizeof(path));
            else if (item->arg && item->kind == 2)
            {
                media_track_t track;
                if (media_library_get_track(item->arg, &track))
                    media_utf8_copy(path, sizeof(path), track.path);
            }
            media_ui_context_menu(item, path[0] ? path : NULL, item->kind == 2 ? item->arg : 0);
            return true;
        }

        switch (item->kind)
        {
        case 0: // Home category
            media_ui_library_enter((media_browse_mode_t)item->arg, 0, item->text);
            break;

        case 1: // Folder
        {
            char path[MEDIA_MAX_PATH + 1];
            if (folder_item_path(item, path, sizeof(path)))
            {
                media_utf8_copy(mui.folder, sizeof(mui.folder), path);
                mui.list.cursor = 0;
                media_ui_library_refresh();
            }
            break;
        }

        case 2: // Track
            play_list_from(mui.list.cursor);
            break;

        case 3: // Album / artist / genre group
        {
            media_browse_mode_t mode = mui.browse == MEDIA_BROWSE_ALBUMS  ? MEDIA_BROWSE_ALBUM_TRACKS
                                       : mui.browse == MEDIA_BROWSE_ARTISTS ? MEDIA_BROWSE_ARTIST_TRACKS
                                                                            : MEDIA_BROWSE_GENRE_TRACKS;
            media_ui_library_enter(mode, item->arg, item->text);
            break;
        }

        case 4: // Playlist
            if (mui.browse == MEDIA_BROWSE_FOLDERS)
            {
                char path[MEDIA_MAX_PATH + 1];
                if (folder_item_path(item, path, sizeof(path)))
                {
                    media_utf8_copy(mui.playlist_path, sizeof(mui.playlist_path), path);
                    media_ui_library_enter(MEDIA_BROWSE_PLAYLIST_TRACKS, 0, item->text);
                }
            }
            else
            {
                // Heap, not stack: the main task only has 12 KB and this array is ~10 KB.
                media_playlist_info_t *found =
                    calloc(MEDIA_UI_MAX_PLAYLISTS, sizeof(media_playlist_info_t));
                if (found)
                {
                    int count = media_playlist_list(media_library_root(), found,
                                                    MEDIA_UI_MAX_PLAYLISTS);
                    if ((int)item->arg < count)
                    {
                        media_utf8_copy(mui.playlist_path, sizeof(mui.playlist_path),
                                        found[item->arg].path);
                        free(found);
                        media_ui_library_enter(MEDIA_BROWSE_PLAYLIST_TRACKS, 0, item->text);
                        return true;
                    }
                    free(found);
                }
            }
            break;

        default:
            break;
        }
        return true;

    default:
        return false;
    }
}

/* -------------------------------------------------------------------------------------- */
/* Drawing                                                                                  */
/* -------------------------------------------------------------------------------------- */

static const char *browse_title(void)
{
    switch (mui.browse)
    {
    case MEDIA_BROWSE_HOME:            return "Media";
    case MEDIA_BROWSE_FOLDERS:         return "Folders";
    case MEDIA_BROWSE_ALBUMS:          return "Albums";
    case MEDIA_BROWSE_ARTISTS:         return "Artists";
    case MEDIA_BROWSE_GENRES:          return "Genres";
    case MEDIA_BROWSE_PLAYLISTS:       return "Playlists";
    case MEDIA_BROWSE_FAVORITES:       return "Favorites";
    case MEDIA_BROWSE_RECENT:          return "Recently Played";
    case MEDIA_BROWSE_ALL_TRACKS:      return "All Tracks";
    case MEDIA_BROWSE_PLAYLIST_TRACKS: return mui.filter_name;
    default:                           return mui.filter_name[0] ? mui.filter_name : "Library";
    }
}

/** Small animated bars marking the row that is currently playing. */
static void draw_playing_marker(int x, int y, int h)
{
    int bar_w = RG_MAX(h / 6, 1);
    int gap = bar_w + 1;
    bool moving = mui.snapshot.state == MEDIA_STATE_PLAYING;

    for (int i = 0; i < 3; ++i)
    {
        int height;
        if (moving)
        {
            // Time-based so it animates identically at any frame rate
            int phase = (int)((mui.frame_us / 90000 + i * 2) % 6);
            height = h / 3 + (phase < 3 ? phase : 6 - phase) * (h / 9);
        }
        else
        {
            height = h / 3;
        }
        height = media_clampi(height, 2, h);
        rg_gui_draw_rect(x + i * gap, y + h - height, bar_w, height, 0, 0, mui.theme.accent);
    }
}

void media_ui_library_draw(void)
{
    media_layout_t *l = &mui.layout;
    media_scan_status_t scan = media_library_scan_status();

    char right[32] = "";
    if (scan.scanning)
        snprintf(right, sizeof(right), "Scanning %u", (unsigned)scan.tracks_found);
    else if (mui.list.count)
        snprintf(right, sizeof(right), "%d/%d", mui.list.cursor + 1, mui.list.count);

    media_ui_draw_header(browse_title(), right);

    // The mini player is drawn first so the list knows how much room it actually has.
    media_ui_draw_mini_player();

    int rows = media_list_visible_rows();
    int row_h = l->line_h + 2;
    int top = l->content_top + l->pad / 2;
    int list_w = l->width - l->pad * 3;

    if (mui.browse == MEDIA_BROWSE_FOLDERS)
    {
        // Breadcrumb, relative to the media root
        const char *root = media_library_root();
        const char *relative = mui.folder;
        size_t root_len = strlen(root);
        if (strncmp(mui.folder, root, root_len) == 0)
            relative = mui.folder[root_len] ? mui.folder + root_len + 1 : "/";

        char crumb[96];
        snprintf(crumb, sizeof(crumb), "/%s", relative);
        rg_gui_draw_text(l->pad * 2, top, list_w, crumb, mui.theme.text_dim, C_TRANSPARENT,
                         RG_TEXT_ALIGN_LEFT);
        top += l->line_h;
        rows = RG_MAX(rows - 1, 1);
    }

    if (mui.list.count == 0)
    {
        const char *title = "No media found";
        char body[160];

        if (scan.scanning)
        {
            snprintf(body, sizeof(body), "Building the library...\n\n%u tracks in %u folders\n\n%s",
                     (unsigned)scan.tracks_found, (unsigned)scan.folders_seen, scan.current);
            title = "Scanning";
        }
        else if (!rg_storage_exists(media_library_root()))
        {
            snprintf(body, sizeof(body), "Copy music into:\n%s", media_library_root());
        }
        else if (mui.browse == MEDIA_BROWSE_FAVORITES)
        {
            snprintf(body, sizeof(body), "Hold %s on a track to add it here.",
                     rg_input_get_key_name(RG_KEY_A));
            title = "No favorites yet";
        }
        else if (!media_library_ready())
        {
            snprintf(body, sizeof(body), "Open the menu and choose\nRescan Library.");
        }
        else
        {
            snprintf(body, sizeof(body), "Nothing here.");
            title = "Empty";
        }

        media_ui_draw_message(title, body);
        media_ui_draw_footer("B: Back   MENU: Options");
        return;
    }

    for (int i = 0; i < rows; ++i)
    {
        int index = mui.list.scroll + i;
        if (index >= mui.list.count)
            break;

        const media_list_item_t *item = &mui.list.items[index];
        bool selected = index == mui.list.cursor;
        int y = top + i * row_h;

        if (selected)
            media_ui_draw_panel(l->pad, y - 1, list_w, row_h, media_color_scale(mui.theme.accent, 60),
                                mui.theme.accent);

        int text_x = l->pad * 2;
        int text_w = list_w - l->pad * 2;

        // Currently playing indicator
        bool playing = item->kind == 2 && item->arg && item->arg == mui.snapshot.track_id;
        if (playing)
        {
            draw_playing_marker(text_x, y + 2, l->line_h - 3);
            text_x += l->line_h;
            text_w -= l->line_h;
        }
        else if (item->kind == 1)
        {
            // Folder glyph
            rg_gui_draw_rect(text_x, y + l->line_h / 3, l->line_h / 2, l->line_h / 2 - 1, 0, 0,
                             mui.theme.accent_dim);
            rg_gui_draw_rect(text_x, y + l->line_h / 4, l->line_h / 4, 2, 0, 0, mui.theme.accent_dim);
            text_x += l->line_h;
            text_w -= l->line_h;
        }

        // Favourite marker: a filled block, so the state does not rely on colour alone
        if (item->flags & MEDIA_ENTRY_FAVORITE)
        {
            int fx = l->pad + list_w - l->pad - 4;
            rg_gui_draw_rect(fx, y + l->line_h / 3, 4, 4, 0, 0, mui.theme.highlight);
            text_w -= 8;
        }

        rg_color_t color = selected ? mui.theme.text : mui.theme.text_dim;
        int sub_w = 0;

        // In the folder view `subtext` carries the on-disk filename so the path can be
        // rebuilt; it is working data, not something worth showing next to the title.
        if (item->subtext[0] && mui.browse != MEDIA_BROWSE_FOLDERS)
        {
            rg_rect_t measured = TEXT_RECT(item->subtext, 0);
            sub_w = RG_MIN(measured.width + l->pad, text_w / 2);
            rg_gui_draw_text(text_x + text_w - sub_w, y, sub_w, item->subtext,
                             selected ? mui.theme.accent : mui.theme.divider, C_TRANSPARENT,
                             RG_TEXT_ALIGN_RIGHT);
        }

        media_ui_draw_marquee(text_x, y, text_w - sub_w, item->text, color, 0, selected);
    }

    media_ui_draw_scrollbar(l->width - l->pad, top, rows * row_h, rows, mui.list.count,
                            mui.list.scroll);

    media_ui_draw_footer("A: Play   B: Back   START: Player   MENU: Options");
}
