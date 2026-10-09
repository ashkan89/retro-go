/* Host framebuffer preview using the production Now Playing layout and drawing helpers,
 * with the repository's Sans 12 glyphs. This is a rendering harness, not a device simulator. */
#include <rg_system.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
static struct tm *localtime_r(const time_t *now, struct tm *result)
{ struct tm *value = localtime(now); if (!value) return NULL; *result = *value; return result; }
#endif
#include "media_player.h"
#include "media_ui_geometry.h"
#include "media_util.h"
#define C_TRANSPARENT -1
#define C_WHITE 65535
#define RG_TEXT_ALIGN_LEFT 2
#define RG_TEXT_ALIGN_CENTER 4
#define RG_TEXT_ALIGN_RIGHT 8
#define RG_TEXT_DUMMY_DRAW 16
#define RG_TEXT_BIGGER 64
typedef struct { int left, top, width, height; } rg_rect_t;
typedef struct { int width, height, pad, line_h, header_h, footer_h, content_top, content_h; } media_layout_t;
typedef struct { rg_color_t background, surface, text, text_dim, accent, accent_dim, highlight, divider; } media_theme_t;
typedef struct { rg_color_t primary; bool valid; } media_palette_t;
typedef struct __attribute__((packed))
{ uint16_t code; uint8_t yOffset, width, height, xOffset, xDelta; uint8_t data[]; } rg_font_glyph_t;
typedef struct { char name[16]; uint8_t type, width, height; size_t chars; uint8_t data[]; } rg_font_t;
#include "media_preview_font.h"
#include "media_preview_glyph.h"
static struct
{
    media_layout_t layout;
    media_theme_t theme;
    media_snapshot_t snapshot;
    media_track_t *track;
    int64_t frame_us, marquee_reset_at;
} mui;
static uint16_t framebuffer[480 * 480];
static int font_height = 16;
static const rg_font_t *preview_font = &font_Sans12;
static void pixel(int x, int y, rg_color_t color)
{
    if (color >= 0 && x >= 0 && y >= 0 && x < mui.layout.width && y < mui.layout.height)
        framebuffer[y * mui.layout.width + x] = (uint16_t)color;
}
static void rg_gui_draw_rect(int x, int y, int w, int h, int border, rg_color_t edge, rg_color_t color)
{
    (void)border; (void)edge;
    for (int dy = 0; dy < h; ++dy) for (int dx = 0; dx < w; ++dx) pixel(x + dx, y + dy, color);
}
int rg_utf8_decode(const char **p)
{
    unsigned char first = (unsigned char)*(*p)++;
    if (first < 128) return first;
    int code = first & ((first < 224) ? 31 : 15), more = first < 224 ? 1 : 2;
    while (more-- && **p) code = (code << 6) | ((unsigned char)*(*p)++ & 63);
    return code;
}
static rg_rect_t rg_gui_draw_text(int x, int y, int width, const char *text,
                                 rg_color_t color, rg_color_t bg, uint32_t flags)
{
    (void)bg;
    int height = flags & RG_TEXT_BIGGER ? font_height * 2 : font_height;
    int measured = 2;
    for (const char *p = text; *p;) measured += (int)get_glyph(NULL, preview_font, height, rg_utf8_decode(&p));
    rg_rect_t rect = {.width = measured, .height = height + 2};
    if (flags & RG_TEXT_DUMMY_DRAW) return rect;
    if (!width) width = measured;
    int cursor = x + 1;
    if (flags & RG_TEXT_ALIGN_RIGHT) cursor = x + width - measured + 1;
    else if (flags & RG_TEXT_ALIGN_CENTER) cursor = x + (width - measured) / 2 + 1;
    for (const char *p = text; *p;)
    {
        uint32_t rows[64] = {0};
        int glyph_w = (int)get_glyph(rows, preview_font, height, rg_utf8_decode(&p));
        for (int gy = 0; gy < height; ++gy) for (int gx = 0; gx < glyph_w; ++gx)
            if (cursor + gx >= x && cursor + gx < x + width && rows[gy] & (1u << gx))
                pixel(cursor + gx, y + gy + 1, color);
        cursor += glyph_w;
    }
    return rect;
}
#define TEXT_RECT(text,max) rg_gui_draw_text(0,0,0,text,0,0,RG_TEXT_DUMMY_DRAW)
size_t media_utf8_copy(char *dst, size_t capacity, const char *src)
{ snprintf(dst, capacity, "%s", src); return strlen(dst); }
const char *rg_basename(const char *path) { const char *last = strrchr(path, '/'); return last ? last + 1 : path; }
const char *media_player_path(void) { return "album/song.mp3"; }
const char *media_player_last_error(void) { return "Unable to open song"; }
const char *media_ui_art_path(void) { return "cover.jpg"; }
const char *media_codec_name(media_codec_t codec) { (void)codec; return "MP3"; }
void media_queue_lock(void) {}
void media_queue_unlock(void) {}
int media_queue_next_index(bool manual) { (void)manual; return 1; }
const char *media_queue_path(int index) { (void)index; return "When the city falls asleep.mp3"; }
void media_path_stem(char *buf, size_t capacity, const char *path)
{ snprintf(buf, capacity, "%s", path); }
void media_format_time(char *buf, size_t capacity, uint32_t ms)
{ snprintf(buf, capacity, "%u:%02u", ms / 60000, ms / 1000 % 60); }
static rg_color_t media_color_scale(rg_color_t c, int scale)
{ return C_RGB(((c >> 11) & 31) * 8 * scale / 255, ((c >> 5) & 63) * 4 * scale / 255, (c & 31) * 8 * scale / 255); }
static media_palette_t media_artwork_palette(const char *path)
{ (void)path; return (media_palette_t){.primary = C_RGB(70,220,190), .valid = true}; }
bool media_library_get_track(uint32_t id, media_track_t *track)
{ (void)id; memset(track, 0, sizeof(*track)); strcpy(track->title, "When the city falls asleep"); return true; }
static void media_ui_draw_art(int x, int y, int size, const char *path, const media_palette_t *p, const char *label)
{
    (void)path; (void)p; (void)label;
    for (int dy = 0; dy < size; ++dy) for (int dx = 0; dx < size; ++dx)
    {
        int wave = abs((dx * 2 + dy * 3) % size - size / 2);
        pixel(x + dx, y + dy, C_RGB(30 + dy * 90 / size, 60 + dx * 120 / size, 110 + wave * 180 / size));
    }
}
static void media_ui_draw_message(const char *title, const char *body) { (void)title; (void)body; }
#include "media_preview_helpers.h"
#include "media_preview_player.h"
static void rg_audio_submit(const rg_audio_frame_t *frames, size_t count) { (void)frames; (void)count; }
int main(int argc, char **argv)
{
    current_route = RG_AUDIO_ROUTE_SPEAKER;
    if (argc < 4) return 1;
    int width = atoi(argv[1]), height = atoi(argv[2]);
    font_height = argc > 4 ? atoi(argv[4]) : 16;
    if (font_height == 19) preview_font = &font_Sans15;
    assert(width <= 480 && height <= 480);
    int pad = RG_MAX(width / 60, 3), line = font_height + 2;
    mui.layout = (media_layout_t){width,height,pad,line,line+pad*2,line+pad,line+pad*2,height-line*2-pad*3};
    mui.theme = (media_theme_t){C_RGB(8,13,23),C_RGB(19,29,43),C_RGB(240,240,244),C_RGB(140,150,165),
        C_RGB(65,220,190),C_RGB(25,90,85),C_RGB(160,205,255),C_RGB(48,58,70)};
    media_track_t track = {0};
    strcpy(track.title, "Midnight on the coastline - extended edition");
    strcpy(track.artist, "The Northern Lights Orchestra");
    strcpy(track.album, "Letters from the ocean");
    mui.track = &track;
    mui.snapshot = (media_snapshot_t){.state=MEDIA_STATE_PLAYING,.duration_ms=245000,.position_ms=83000,
        .sample_rate=44100,.bitrate=192000,.volume=75,.favorite=true,.next_track_id=1};
    mui.frame_us = 16000000;
    rg_gui_draw_rect(0,0,width,height,0,0,mui.theme.background);
    media_ui_nowplaying_draw();
    FILE *file = fopen(argv[3], "wb");
    assert(file); fprintf(file,"P6\n%d %d\n255\n",width,height);
    for (int i = 0; i < width * height; ++i)
    {
        uint16_t c = framebuffer[i];
        unsigned char rgb[3] = {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
        fwrite(rgb,1,3,file);
    }
    fclose(file);
    return 0;
}
