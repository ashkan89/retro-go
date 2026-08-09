/**
 * Retro-Go media player - network sources.
 *
 * Two things live here:
 *
 *  - URL handling, so an http(s) address can be used anywhere a path can (queue entries,
 *    playlists, the decoder's source layer).
 *  - Remote folder listing, so a shared folder on a NAS or a PC can be browsed exactly like
 *    /media on the card. Two listing dialects are understood: WebDAV PROPFIND (Nextcloud,
 *    Synology, most NAS boxes, `rclone serve webdav`) and the plain HTML directory index
 *    that nginx, Apache and `python3 -m http.server` produce.
 *
 * There is no SMB/CIFS client: ESP-IDF does not ship one, and vendoring an SMB stack for
 * this is far more surface area than an HTTP listing needs. Anything that can be exposed
 * over HTTP or WebDAV works today.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_types.h"

#define MEDIA_NET_MAX_ENTRIES 128
#define MEDIA_NET_NAME_LEN 96

typedef struct
{
    char name[MEDIA_NET_NAME_LEN];
    char url[MEDIA_MAX_PATH + 1];
    bool is_dir;
    bool is_stream;     // A bookmark the user added as a radio station rather than a folder
} media_net_entry_t;

/** True when this build has networking and the interface is currently connected. */
bool media_net_available(void);

/** Human-readable reason the network is not usable, or NULL when it is. */
const char *media_net_status(void);

bool media_net_is_url(const char *path);

/**
 * Copy the display name for a URL (last path segment, percent-decoded, query stripped).
 * Falls back to the host name for a bare "http://host/" style address.
 */
void media_net_display_name(char *out, size_t out_size, const char *url);

/** Join a base URL and a (possibly relative or absolute) href into an absolute URL. */
bool media_net_resolve(char *out, size_t out_size, const char *base_url, const char *href);

/**
 * List a remote folder. Tries WebDAV first, then an HTML index. Entries are sorted with
 * folders first. Returns the number written, or -1 when the server could not be reached.
 */
int media_net_list(const char *url, media_net_entry_t *out, int max);

/* ---- Bookmarks ---------------------------------------------------------------------- */

/**
 * Saved servers and stations, stored as a plain tab-separated file at
 * `<root>/.retrogo-media/network.txt` so it can be written by hand rather than typed in on
 * an on-screen keyboard.
 */
int media_net_bookmarks_load(media_net_entry_t *out, int max);
bool media_net_bookmark_add(const char *name, const char *url, bool is_dir, bool is_stream);
bool media_net_bookmark_remove(const char *url);
