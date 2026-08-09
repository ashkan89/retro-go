/**
 * Retro-Go media player - internal UI contract.
 *
 * Every screen renders into one shared off-screen surface and returns; the run loop owns
 * timing, input and the single submit per frame. Screens never read the decoder directly:
 * they get the snapshot the loop captured at the top of the frame.
 */
#pragma once

#include <rg_gui.h>
#include <rg_system.h>

#include "media.h"
#include "media_artwork.h"
#include "media_image.h"
#include "media_library.h"
#include "media_player.h"
#include "media_queue.h"
#include "media_settings.h"
#include "media_types.h"
#include "media_util.h"

typedef struct
{
    int width, height;
    int pad;
    int line_h;
    int header_h;
    int footer_h;
    int content_top;
    int content_h;
    int mini_h;         // Mini-player strip height (0 when hidden)
    rg_margins_t safe;
} media_layout_t;

typedef struct
{
    rg_color_t background;
    rg_color_t surface;     // Card / panel fill
    rg_color_t text;
    rg_color_t text_dim;
    rg_color_t accent;
    rg_color_t accent_dim;
    rg_color_t highlight;
    rg_color_t divider;
} media_theme_t;

/* media_browse_mode_t lives in media.h so the launcher and the UI share one list. */

#define MEDIA_LIST_TEXT 96
#define MEDIA_UI_MAX_PLAYLISTS 32

/**
 * What a browser row represents. `arg` means something different per kind: a library track
 * id for ROW_TRACK, a group hash for ROW_GROUP, and an index into the cached remote listing
 * for the ROW_NET_* kinds (a URL does not come close to fitting in a row).
 */
typedef enum
{
    ROW_ACTION = 0,     // Home category / navigation
    ROW_FOLDER,         // Folder on the card
    ROW_TRACK,          // Track in the library
    ROW_GROUP,          // Album / artist / genre
    ROW_PLAYLIST,
    ROW_NET_ADD,        // "Add a network location..."
    ROW_NET_FOLDER,     // Remote folder
    ROW_NET_TRACK,      // Remote file or radio stream
} media_row_kind_t;

typedef struct
{
    char text[MEDIA_LIST_TEXT];
    char subtext[MEDIA_LIST_TEXT / 2];
    uint32_t arg;
    uint8_t kind;       // media_row_kind_t
    uint8_t flags;
} media_list_item_t;

typedef struct
{
    media_list_item_t *items;
    int count;
    int capacity;
    int cursor;
    int scroll;
} media_list_t;

/** Everything the UI owns for one session. */
typedef struct
{
    rg_surface_t *surface;
    media_layout_t layout;
    media_theme_t theme;

    media_page_t page;
    media_browse_mode_t browse;
    bool in_library;

    media_list_t list;

    /* Browser navigation state */
    char folder[MEDIA_MAX_PATH + 1];
    uint32_t filter_hash;
    char filter_name[MEDIA_TAG_ALBUM_LEN];
    char playlist_path[MEDIA_MAX_PATH + 1];

    /* Snapshot captured at the start of the frame */
    media_snapshot_t snapshot;
    const media_track_t *track;

    /* Transient overlays */
    int64_t overlay_until_us;
    char overlay_title[32];
    char overlay_value[48];
    int overlay_percent;         // -1 when the overlay has no bar
    int32_t seek_preview_ms;     // Non-zero while a seek is being previewed

    /* Animation */
    media_anim_t progress_anim;
    media_anim_t lyric_anim;
    int64_t frame_us;
    int64_t last_frame_us;
    int64_t marquee_reset_at;
    int lyric_index;

    /* Quick-settings overlay */
    bool quick_open;
    int quick_row;

    bool needs_redraw;
    bool running;
    uint32_t frames;
    float fps;
} media_ui_t;

extern media_ui_t mui;

/* ---- Core helpers (media_ui.c) ------------------------------------------------------- */

void media_ui_update_theme(void);
void media_ui_clear(void);
void media_ui_draw_header(const char *title, const char *right);
void media_ui_draw_footer(const char *hints);
void media_ui_draw_panel(int x, int y, int w, int h, rg_color_t fill, rg_color_t border);
void media_ui_draw_progress(int x, int y, int w, int h, int percent, rg_color_t fill, rg_color_t bg);
void media_ui_draw_marquee(int x, int y, int w, const char *text, rg_color_t color, uint32_t flags,
                           bool active);
void media_ui_draw_scrollbar(int x, int y, int h, int visible, int total, int offset);
void media_ui_show_overlay(const char *title, const char *value, int percent);
void media_ui_draw_overlay(void);
void media_ui_draw_mini_player(void);
void media_ui_draw_message(const char *title, const char *body);
void media_ui_draw_art(int x, int y, int size, const char *path, const media_palette_t *palette,
                       const char *fallback_text);

void media_list_reset(media_list_t *list);
media_list_item_t *media_list_add(media_list_t *list);
void media_list_free(media_list_t *list);
void media_list_move(media_list_t *list, int delta, int page);
int media_list_visible_rows(void);

/* ---- Screens ------------------------------------------------------------------------- */

void media_ui_run(void);
void media_ui_set_pending_view(media_browse_mode_t mode);
void media_ui_library_enter(media_browse_mode_t mode, uint32_t filter_hash, const char *name);
void media_ui_library_refresh(void);
void media_ui_library_draw(void);
/** Returns true when the key was consumed. */
bool media_ui_library_input(uint32_t key, bool repeat);
bool media_ui_library_back(void);

void media_ui_nowplaying_draw(void);
void media_ui_lyrics_draw(void);
void media_ui_visualizer_draw(void);
void media_ui_queue_draw(void);
void media_ui_queue_refresh(void);
bool media_ui_queue_input(uint32_t key);
void media_ui_info_draw(void);
bool media_ui_info_input(uint32_t key);

void media_ui_equalizer_screen(void);
void media_ui_visualizer_menu(void);
void media_ui_player_menu(void);
void media_ui_settings_menu(void);
void media_ui_context_menu(const media_list_item_t *item, const char *path, uint32_t track_id);

/** Prompt for a URL and either browse it, save it as a station, or play it right away. */
void media_ui_network_add(void);

/** Release the cached remote listing (on leaving the player). */
void media_ui_library_release(void);
