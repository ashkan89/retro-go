#pragma once
#include <stdbool.h>

/* Pure geometry shared with native overlap tests. All bottom controls reserve their rows
 * before artwork/text are placed. Optional details disappear before mandatory controls. */
typedef struct
{
    int status_y, hero_y, hero_h, quality_y, next_y, bar_y, time_y, transport_y, transport_h;
    int art_x, art_y, art_size, text_x, text_y, text_w, title_h;
    bool large_title, album;
} media_player_geometry_t;

static inline media_player_geometry_t media_player_geometry(int width, int top, int bottom,
                                                           int line, int pad)
{
    media_player_geometry_t g = {0};
    g.status_y = top + pad;
    g.transport_h = line + pad * 2;
    g.transport_y = bottom - g.transport_h;
    g.time_y = g.transport_y - line - pad;
    g.bar_y = g.time_y - 8;
    g.next_y = g.bar_y - line - pad;
    g.quality_y = g.next_y - line;
    g.hero_y = g.status_y + line + pad;
    g.hero_h = g.quality_y - pad - g.hero_y;
    if (g.hero_h < line * 3)
    {
        g.next_y = -1;
        g.quality_y = g.bar_y - line - pad;
        g.hero_h = g.quality_y - pad - g.hero_y;
    }
    if (g.hero_h < line * 3)
    {
        g.quality_y = -1;
        g.hero_h = g.bar_y - pad - g.hero_y;
    }
    if (g.hero_h < 0) g.hero_h = 0;
    g.large_title = g.hero_h >= line * 4;
    g.title_h = g.large_title ? line * 2 : line;
    int metadata_h = g.title_h + line;
    g.album = g.hero_h >= metadata_h + line;
    if (g.album) metadata_h += line;
    g.text_x = pad * 2;
    g.text_w = width - pad * 4;
    g.text_y = g.hero_y;
    if (width >= 280 && g.hero_h >= line * 3)
    {
        g.art_size = g.hero_h;
        if (g.art_size > width * 2 / 5) g.art_size = width * 2 / 5;
        g.art_x = pad * 2;
        g.art_y = g.hero_y + (g.hero_h - g.art_size) / 2;
        g.text_x = g.art_x + g.art_size + pad * 2;
        g.text_w = width - g.text_x - pad * 2;
        g.text_y = g.hero_y + (g.hero_h - metadata_h) / 2;
    }
    else if (g.hero_h > metadata_h + line * 2 + pad)
    {
        g.art_size = g.hero_h - metadata_h - pad;
        if (g.art_size > width / 2) g.art_size = width / 2;
        g.art_x = (width - g.art_size) / 2;
        g.art_y = g.hero_y;
        g.text_y = g.hero_y + g.art_size + pad;
    }
    return g;
}
