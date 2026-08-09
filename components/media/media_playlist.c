#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_playlist.h"
#include "media_util.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_DB"

static bool playlist_add(media_playlist_t *pl, const char *path)
{
    size_t len = strlen(path);
    if (!len || len > MEDIA_MAX_PATH)
        return false;
    if (pl->count >= MEDIA_MAX_PLAYLIST_ENTRIES)
        return false;

    if (pl->count >= pl->capacity)
    {
        int capacity = pl->capacity ? pl->capacity * 2 : 64;
        if (capacity > MEDIA_MAX_PLAYLIST_ENTRIES)
            capacity = MEDIA_MAX_PLAYLIST_ENTRIES;
        uint32_t *offsets = realloc(pl->offsets, (size_t)capacity * sizeof(uint32_t));
        if (!offsets)
            return false;
        pl->offsets = offsets;
        pl->capacity = capacity;
    }

    if (pl->pool_used + len + 1 > pl->pool_capacity)
    {
        size_t capacity = pl->pool_capacity ? pl->pool_capacity * 2 : 4096;
        while (capacity < pl->pool_used + len + 1)
            capacity *= 2;
        if (capacity > (size_t)MEDIA_MAX_PLAYLIST_ENTRIES * 96)
            return false;
        char *pool = realloc(pl->pool, capacity);
        if (!pool)
            return false;
        pl->pool = pool;
        pl->pool_capacity = capacity;
    }

    pl->offsets[pl->count++] = (uint32_t)pl->pool_used;
    memcpy(pl->pool + pl->pool_used, path, len + 1);
    pl->pool_used += len + 1;
    return true;
}

media_playlist_t *media_playlist_load(const char *path, const char *root)
{
    if (!path || !*path)
        return NULL;

    rg_stat_t info = rg_storage_stat(path);
    if (!info.exists || !info.is_file || info.size == 0)
        return NULL;
    // A playlist is a text file; anything this large is not one.
    if (info.size > (size_t)MEDIA_MAX_PLAYLIST_ENTRIES * (MEDIA_MAX_PATH + 2))
        return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    media_playlist_t *pl = calloc(1, sizeof(media_playlist_t));
    if (!pl)
    {
        fclose(fp);
        return NULL;
    }

    media_path_stem(pl->name, sizeof(pl->name), path);

    char base_dir[MEDIA_MAX_PATH + 1];
    media_utf8_copy(base_dir, sizeof(base_dir), rg_dirname(path));

    char line[MEDIA_MAX_PATH * 2 + 4];
    char resolved[MEDIA_MAX_PATH + 1];
    int rejected = 0;

    while (fgets(line, sizeof(line), fp))
    {
        media_str_trim(line);

        if (!line[0])
            continue;
        if (line[0] == '#') // #EXTM3U, #EXTINF and friends carry no path
            continue;

        // Skip a UTF-8 BOM on the very first entry
        char *entry = line;
        if ((uint8_t)entry[0] == 0xEF && (uint8_t)entry[1] == 0xBB && (uint8_t)entry[2] == 0xBF)
            entry += 3;

        // Remote URLs are meaningless here
        if (strstr(entry, "://"))
            continue;

        if (!media_path_resolve(resolved, sizeof(resolved), base_dir, entry, root))
        {
            rejected++;
            continue;
        }

        if (!media_path_is_audio(resolved))
            continue;

        if (!playlist_add(pl, resolved))
            break;
    }

    fclose(fp);

    if (rejected)
        RG_LOGW("Playlist '%s': %d entries were outside the media root", pl->name, rejected);

    if (pl->count == 0)
    {
        media_playlist_free(pl);
        return NULL;
    }

    RG_LOGI("Loaded playlist '%s' with %d tracks", pl->name, pl->count);
    return pl;
}

void media_playlist_free(media_playlist_t *playlist)
{
    if (!playlist)
        return;
    free(playlist->pool);
    free(playlist->offsets);
    free(playlist);
}

const char *media_playlist_entry(const media_playlist_t *playlist, int index)
{
    if (!playlist || index < 0 || index >= playlist->count)
        return NULL;
    return playlist->pool + playlist->offsets[index];
}

bool media_playlist_save(const char *path, const char *const *paths, int count)
{
    if (!path || !paths || count <= 0)
        return false;
    if (count > MEDIA_MAX_PLAYLIST_ENTRIES)
        count = MEDIA_MAX_PLAYLIST_ENTRIES;

    // Build the whole file in memory so the write can be atomic.
    size_t capacity = 16;
    for (int i = 0; i < count; ++i)
        capacity += paths[i] ? strlen(paths[i]) + 2 : 0;

    char *buffer = malloc(capacity);
    if (!buffer)
        return false;

    size_t used = 0;
    used += (size_t)snprintf(buffer + used, capacity - used, "#EXTM3U\n");

    for (int i = 0; i < count; ++i)
    {
        if (!paths[i] || !paths[i][0])
            continue;
        int n = snprintf(buffer + used, capacity - used, "%s\n", paths[i]);
        if (n < 0 || (size_t)n >= capacity - used)
            break;
        used += (size_t)n;
    }

    bool ok = rg_storage_write_file(path, buffer, used, RG_FILE_ATOMIC_WRITE);
    free(buffer);

    if (ok)
        RG_LOGI("Saved playlist '%s' (%d tracks)", rg_basename(path), count);
    else
        RG_LOGE("Failed to save playlist '%s'", path);

    return ok;
}

bool media_playlist_append(const char *path, const char *track_path)
{
    if (!path || !track_path)
        return false;

    // Appending is a small, frequent operation; do it in place rather than rewriting.
    FILE *fp = fopen(path, "ab");
    if (!fp && rg_storage_mkdir(rg_dirname(path)))
        fp = fopen(path, "ab");
    if (!fp)
        return false;

    if (ftell(fp) == 0)
        fputs("#EXTM3U\n", fp);

    fputs(track_path, fp);
    fputc('\n', fp);
    bool ok = ferror(fp) == 0;
    fclose(fp);

    return ok;
}

typedef struct
{
    media_playlist_info_t *out;
    int max;
    int count;
} playlist_scan_t;

static int playlist_scan_cb(const rg_scandir_t *file, void *arg)
{
    playlist_scan_t *state = arg;

    if (state->count >= state->max)
        return RG_SCANDIR_STOP;
    if (!file->is_file || media_path_is_hidden(file->basename))
        return RG_SCANDIR_CONTINUE;
    if (!media_path_is_playlist(file->basename))
        return RG_SCANDIR_CONTINUE;
    if (strlen(file->path) > MEDIA_MAX_PATH)
        return RG_SCANDIR_CONTINUE;

    media_playlist_info_t *info = &state->out[state->count++];
    strcpy(info->path, file->path);
    media_path_stem(info->name, sizeof(info->name), file->path);

    return RG_SCANDIR_CONTINUE;
}

int media_playlist_list(const char *root, media_playlist_info_t *out, int max)
{
    if (!root || !out || max <= 0)
        return 0;

    playlist_scan_t state = {out, max, 0};

    char folder[MEDIA_MAX_PATH + 1];
    if (media_path_join(folder, sizeof(folder), root, "Playlists") && rg_storage_exists(folder))
        rg_storage_scandir(folder, playlist_scan_cb, &state, RG_SCANDIR_FILES | RG_SCANDIR_SORT);

    // Playlists dropped straight into the media root are common enough to be worth finding.
    rg_storage_scandir(root, playlist_scan_cb, &state, RG_SCANDIR_FILES | RG_SCANDIR_SORT);

    return state.count;
}
