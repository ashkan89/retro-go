/**
 * Media launcher tab.
 *
 * The tab is a thin shortcut list: selecting a row hands the display over to the media
 * component and gets it back when the user leaves. All of the player's own state lives in
 * components/media, so the launcher stays unaware of codecs, queues and artwork.
 */
#include <rg_system.h>

#include <stdio.h>
#include <string.h>

#include <media.h>

#include "gui.h"
#include "media_tab.h"

static tab_t *media_tab;

/* Row identifiers double as the browser view the player should open on. `arg` is stored as
 * a pointer-sized value, which is why the enum is offset by one from a null pointer. */
enum
{
    ROW_OPEN = 1,
    ROW_NOW_PLAYING,
    ROW_FOLDERS,
    ROW_ALBUMS,
    ROW_ARTISTS,
    ROW_PLAYLISTS,
    ROW_FAVORITES,
    ROW_RECENT,
    ROW_RESCAN,
};

static void tab_refresh(tab_t *tab)
{
    char line[92];
    int row = 0;

    gui_resize_list(tab, 9);
    tab->listbox.sort_mode = SORT_NONE;

    media_status_line(line, sizeof(line));

    snprintf(tab->listbox.items[row].text, sizeof(tab->listbox.items[row].text),
             "%s", _("Open Media Player"));
    tab->listbox.items[row].arg = (void *)(intptr_t)ROW_OPEN;
    tab->listbox.items[row].order = row;
    row++;

    if (line[0])
        snprintf(tab->listbox.items[row].text, sizeof(tab->listbox.items[row].text), "%s", line);
    else
        snprintf(tab->listbox.items[row].text, sizeof(tab->listbox.items[row].text), "%s",
                 _("Nothing playing"));
    tab->listbox.items[row].arg = (void *)(intptr_t)ROW_NOW_PLAYING;
    tab->listbox.items[row].order = row;
    row++;

    static const struct
    {
        const char *label;
        int id;
    } shortcuts[] = {
        {"Folders",         ROW_FOLDERS},
        {"Albums",          ROW_ALBUMS},
        {"Artists",         ROW_ARTISTS},
        {"Playlists",       ROW_PLAYLISTS},
        {"Favorites",       ROW_FAVORITES},
        {"Recently Played", ROW_RECENT},
        {"Rescan Library",  ROW_RESCAN},
    };

    for (size_t i = 0; i < RG_COUNT(shortcuts) && row < tab->listbox.length; ++i)
    {
        snprintf(tab->listbox.items[row].text, sizeof(tab->listbox.items[row].text), "%s",
                 _(shortcuts[i].label));
        tab->listbox.items[row].arg = (void *)(intptr_t)shortcuts[i].id;
        tab->listbox.items[row].order = row;
        row++;
    }

    gui_resize_list(tab, row);

    media_library_line(line, sizeof(line));
    gui_set_status(tab, line, media_is_playing() ? "Playing" : "");
}

static void event_handler(gui_event_t event, tab_t *tab)
{
    listbox_item_t *item = gui_get_selected_item(tab);
    int row = item ? (int)(intptr_t)item->arg : 0;

    switch (event)
    {
    case TAB_INIT:
        media_init();
        tab_refresh(tab);
        break;

    case TAB_REFRESH:
        tab_refresh(tab);
        break;

    case TAB_ENTER:
    case TAB_LEAVE:
        gui_set_preview(tab, NULL);
        tab_refresh(tab);
        break;

    case TAB_IDLE:
        // The status line changes as tracks advance; refresh it about once a second rather
        // than on every idle tick.
        if (gui.idle_counter % 10 == 1)
            tab_refresh(tab);
        break;

    case TAB_ACTION:
        switch (row)
        {
        case ROW_OPEN:
        case ROW_NOW_PLAYING:
            media_run();
            break;
        case ROW_FOLDERS:
            media_run_at(MEDIA_BROWSE_FOLDERS);
            break;
        case ROW_ALBUMS:
            media_run_at(MEDIA_BROWSE_ALBUMS);
            break;
        case ROW_ARTISTS:
            media_run_at(MEDIA_BROWSE_ARTISTS);
            break;
        case ROW_PLAYLISTS:
            media_run_at(MEDIA_BROWSE_PLAYLISTS);
            break;
        case ROW_FAVORITES:
            media_run_at(MEDIA_BROWSE_FAVORITES);
            break;
        case ROW_RECENT:
            media_run_at(MEDIA_BROWSE_RECENT);
            break;
        case ROW_RESCAN:
            media_run_at(MEDIA_BROWSE_HOME);
            break;
        default:
            break;
        }
        gui_invalidate_media_tab();
        break;

    default:
        break;
    }
}

void media_tab_init(void)
{
    // The tab name doubles as the key for its theme images (logo_mediaplayer.png,
    // banner_mediaplayer.png, background_mediaplayer.png) and for its hide-tab setting, so
    // it follows the short lowercase form the emulator tabs use.
    media_tab = gui_add_tab("mediaplayer", "Media Player", NULL, event_handler);
}

void gui_invalidate_media_tab(void)
{
    if (media_tab)
        tab_refresh(media_tab);
}
