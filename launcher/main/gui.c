#include <rg_system.h>
#include <string.h>
#include <stdlib.h>

#include "applications.h"
#include "gui.h"

/* Width reserved to the right of the list for the scrollbar, and how far a row's text sits from
 * the edge of its selection pill (past the accent bar). */
#define SCROLLBAR_GUTTER    (6)
#define ROW_TEXT_INSET      (8)

retro_gui_t gui;

static int max_visible_lines(const tab_t *tab, int *_line_height);

#define SETTING_SELECTED_TAB    "SelectedTab"
#define SETTING_START_SCREEN    "StartScreen"
#define SETTING_STARTUP_MODE    "StartupMode"
#define SETTING_LANGUAGE        "Language"
#define SETTING_COLOR_THEME     "ColorTheme"
#define SETTING_SHOW_PREVIEW    "ShowPreview"
#define SETTING_SCROLL_MODE     "ScrollMode"
#define SETTING_SCREEN_DIM      "ScreenDimTimeout"
#define SETTING_SCREEN_OFF      "ScreenOffTimeout"
#define SETTING_HIDE_TAB(name)  strcat((char[99]){"HideTab."}, (name))

void gui_init(bool cold_boot)
{
    gui = (retro_gui_t){
        .selected_tab = rg_settings_get_number(NS_APP, SETTING_SELECTED_TAB, 0),
        .startup_mode = rg_settings_get_number(NS_APP, SETTING_STARTUP_MODE, 0),
        .language     = rg_settings_get_number(NS_APP, SETTING_LANGUAGE, 0),
        .color_theme  = rg_settings_get_number(NS_APP, SETTING_COLOR_THEME, 0),
        .start_screen = rg_settings_get_number(NS_APP, SETTING_START_SCREEN, START_SCREEN_AUTO),
        .show_preview = rg_settings_get_number(NS_APP, SETTING_SHOW_PREVIEW, PREVIEW_MODE_SAVE_COVER),
        .scroll_mode  = rg_settings_get_number(NS_APP, SETTING_SCROLL_MODE, SCROLL_MODE_CENTER),
        .screen_dim_timeout = rg_settings_get_number(NS_APP, SETTING_SCREEN_DIM, 30),
        .screen_off_timeout = rg_settings_get_number(NS_APP, SETTING_SCREEN_OFF, 10),
        .width        = rg_display_get_width(),
        .height       = rg_display_get_height(),
    };
    // Auto: Show carousel on cold boot, browser on warm boot (after cleanly exiting an emulator)
    gui.browse = gui.start_screen == START_SCREEN_BROWSER || (gui.start_screen == START_SCREEN_AUTO && !cold_boot);
    gui.theme = &gui.themes[gui.color_theme % RG_COUNT(gui.themes)];
    gui.http_lock = false;
    gui.low_memory_mode = rg_system_get_app()->lowMemoryMode;
    gui.surface = rg_surface_create(gui.width, gui.height, RG_PIXEL_565_LE, MEM_SLOW);
    gui_update_theme();
}

void gui_event(gui_event_t event, tab_t *tab)
{
    if (tab && tab->event_handler)
        (*tab->event_handler)(event, tab);
}

tab_t *gui_add_tab(const char *name, const char *desc, void *arg, void *event_handler)
{
    RG_ASSERT_ARG(name && desc);

    tab_t *tab = calloc(1, sizeof(tab_t));

    snprintf(tab->name, sizeof(tab->name), "%s", name);
    snprintf(tab->desc, sizeof(tab->desc), "%s", desc);
    sprintf(tab->status[1].left, "Loading...");

    tab->event_handler = event_handler;
    tab->initialized = false;
    tab->enabled = !rg_settings_get_number(NS_APP, SETTING_HIDE_TAB(name), 0);
    tab->arg = arg;
    tab->listbox = (listbox_t){
        .items = calloc(10, sizeof(listbox_item_t)),
        .capacity = 10,
        .length = 0,
        .cursor = 0,
        .sort_mode = SORT_TEXT_ASC,
    };

    gui.tabs[gui.tabs_count++] = tab;

    RG_LOGI("Tab '%s' added at index %d\n", tab->name, gui.tabs_count - 1);

    return tab;
}

void gui_init_tab(tab_t *tab)
{
    if (!tab || tab->initialized)
        return;

    tab->initialized = true;
    // tab->status[0] = 0;

    gui_event(TAB_INIT, tab);
    gui_scroll_list(tab, SCROLL_SET, tab->listbox.cursor);
}

void gui_deinit_tab(tab_t *tab)
{
    if (!tab || !tab->initialized)
        return;

    // FIXME: Maybe the other images should be freed too?
    gui_event(TAB_DEINIT, tab);
    gui_set_preview(tab, NULL);
    gui_resize_list(tab, 10);

    tab->initialized = false;
}

tab_t *gui_get_tab(int index)
{
    return (index >= 0 && index < gui.tabs_count) ? gui.tabs[index] : NULL;
}

void gui_invalidate(void)
{
    for (size_t i = 0; i < gui.tabs_count; ++i)
        gui_deinit_tab(gui.tabs[i]);
    // Kick the user out of the tab and only re-init upon manual re-entry
    gui.browse = false;
    // gui_init_tab(gui_get_current_tab());
}

rg_image_t *gui_get_image(const char *type, const char *subtype)
{
    char name[64];

    if (gui.low_memory_mode)
        return NULL;

    if (subtype && *subtype)
        snprintf(name, sizeof(name), "%s_%s.png", type, subtype);
    else
        snprintf(name, sizeof(name), "%s.png", type);

    // Try to get image from theme
    rg_image_t *img = rg_gui_get_theme_image(name);
    if (img)
        return img;

    // Then fallback to built-in images
    for (const binfile_t **img = builtin_images; *img; img++)
    {
        if (strcmp((*img)->name, name) == 0)
            return rg_surface_load_image((*img)->data, (*img)->size, 0);
    }

    return NULL;
}

tab_t *gui_get_current_tab(void)
{
    tab_t *tab = gui_get_tab(gui.selected_tab);
    if (!tab)
        RG_LOGE("current tab is NULL!");
    return tab;
}

tab_t *gui_set_current_tab(int index)
{
    tab_t *prev_tab = gui_get_tab(gui.selected_tab);
    tab_t *curr_tab;

    index %= (int)gui.tabs_count;

    if (index < 0)
        index += gui.tabs_count;

    gui.selected_tab = index;

    curr_tab = gui_get_tab(gui.selected_tab);

    if (prev_tab && prev_tab != curr_tab)
    {
        // FIXME: We should recompress the images rather than fully free them, because if a custom theme is
        //        used then it means that we're constantly reloading from SD Card which is very slow...
        rg_surface_free(prev_tab->background), prev_tab->background = NULL;
        // rg_surface_free(prev_tab->banner), prev_tab->banner = NULL;
        // rg_surface_free(prev_tab->logo), prev_tab->logo = NULL;
    }

    return curr_tab;
}

void gui_set_status(tab_t *tab, const char *left, const char *right)
{
    if (!tab)
        tab = gui_get_current_tab();
    if (tab && left)
        strcpy(tab->status[1].left, left);
    if (tab && right)
        strcpy(tab->status[1].right, right);
}

void gui_update_theme(void)
{
    // Load our four color schemes from gui theme
    gui.themes[0].background = rg_gui_get_theme_color("launcher_1", "background", C_RGB(14, 15, 20));
    gui.themes[0].foreground = rg_gui_get_theme_color("launcher_1", "foreground", C_RGB(242, 245, 250));
    gui.themes[0].list.standard_bg = rg_gui_get_theme_color("launcher_1", "list_standard_bg", C_TRANSPARENT);
    gui.themes[0].list.standard_fg = rg_gui_get_theme_color("launcher_1", "list_standard_fg", C_RGB(172, 178, 194));
    gui.themes[0].list.selected_bg = rg_gui_get_theme_color("launcher_1", "list_selected_bg", C_TRANSPARENT);
    gui.themes[0].list.selected_fg = rg_gui_get_theme_color("launcher_1", "list_selected_fg", C_RGB(255, 255, 255));

    gui.themes[1].background = rg_gui_get_theme_color("launcher_2", "background", C_RGB(10, 18, 14));
    gui.themes[1].foreground = rg_gui_get_theme_color("launcher_2", "foreground", C_RGB(236, 250, 240));
    gui.themes[1].list.standard_bg = rg_gui_get_theme_color("launcher_2", "list_standard_bg", C_TRANSPARENT);
    gui.themes[1].list.standard_fg = rg_gui_get_theme_color("launcher_2", "list_standard_fg", C_RGB(150, 176, 158));
    gui.themes[1].list.selected_bg = rg_gui_get_theme_color("launcher_2", "list_selected_bg", C_TRANSPARENT);
    gui.themes[1].list.selected_fg = rg_gui_get_theme_color("launcher_2", "list_selected_fg", C_RGB(120, 240, 150));

    gui.themes[2].background = rg_gui_get_theme_color("launcher_3", "background", C_BLACK);
    gui.themes[2].foreground = rg_gui_get_theme_color("launcher_3", "foreground", C_SNOW);
    gui.themes[2].list.standard_bg = rg_gui_get_theme_color("launcher_3", "list_standard_bg", C_TRANSPARENT);
    gui.themes[2].list.standard_fg = rg_gui_get_theme_color("launcher_3", "list_standard_fg", C_GRAY);
    gui.themes[2].list.selected_bg = rg_gui_get_theme_color("launcher_3", "list_selected_bg", C_WHITE);
    gui.themes[2].list.selected_fg = rg_gui_get_theme_color("launcher_3", "list_selected_fg", C_BLACK);

    gui.themes[3].background = rg_gui_get_theme_color("launcher_4", "background", C_BLACK);
    gui.themes[3].foreground = rg_gui_get_theme_color("launcher_4", "foreground", C_SNOW);
    gui.themes[3].list.standard_bg = rg_gui_get_theme_color("launcher_4", "list_standard_bg", C_TRANSPARENT);
    gui.themes[3].list.standard_fg = rg_gui_get_theme_color("launcher_4", "list_standard_fg", C_RGB(190, 196, 210));
    gui.themes[3].list.selected_bg = rg_gui_get_theme_color("launcher_4", "list_selected_bg", C_WHITE);
    gui.themes[3].list.selected_fg = rg_gui_get_theme_color("launcher_4", "list_selected_fg", C_BLACK);

    // Flush our image cache to make sure the new images are loaded next time
    for (size_t i = 0; i < gui.tabs_count; ++i)
    {
        tab_t *tab = gui.tabs[i];
        rg_surface_free(tab->background), tab->background = NULL;
        rg_surface_free(tab->banner), tab->banner = NULL;
        rg_surface_free(tab->logo), tab->logo = NULL;
    }
}

void gui_save_config(void)
{
    rg_settings_set_number(NS_APP, SETTING_SELECTED_TAB, gui.selected_tab);
    rg_settings_set_number(NS_APP, SETTING_START_SCREEN, gui.start_screen);
    rg_settings_set_number(NS_APP, SETTING_SHOW_PREVIEW, gui.show_preview);
    rg_settings_set_number(NS_APP, SETTING_SCROLL_MODE, gui.scroll_mode);
    rg_settings_set_number(NS_APP, SETTING_SCREEN_DIM, gui.screen_dim_timeout);
    rg_settings_set_number(NS_APP, SETTING_SCREEN_OFF, gui.screen_off_timeout);
    rg_settings_set_number(NS_APP, SETTING_COLOR_THEME, gui.color_theme);
    rg_settings_set_number(NS_APP, SETTING_STARTUP_MODE, gui.startup_mode);
    for (int i = 0; i < gui.tabs_count; i++)
        rg_settings_set_number(NS_APP, SETTING_HIDE_TAB(gui.tabs[i]->name), !gui.tabs[i]->enabled);
    rg_settings_commit();
}

listbox_item_t *gui_get_selected_item(tab_t *tab)
{
    if (tab && gui.browse)
    {
        listbox_t *list = &tab->listbox;
        if (list->cursor >= 0 && list->cursor < list->length)
            return &list->items[list->cursor];
    }
    return NULL;
}

static int list_comp_text_asc(const listbox_item_t *a, const listbox_item_t *b)
{
    return a->group == b->group ? strcasecmp(a->text, b->text) : ((int)a->group - b->group);
}

static int list_comp_text_desc(const listbox_item_t *a, const listbox_item_t *b)
{
    return a->group == b->group ? strcasecmp(b->text, a->text) : ((int)a->group - b->group);
}

static int list_comp_id_asc(const listbox_item_t *a, const listbox_item_t *b)
{
    return a->group == b->group ? ((int)a->order - b->order) : ((int)a->group - b->group);
}

static int list_comp_id_desc(const listbox_item_t *a, const listbox_item_t *b)
{
    return a->group == b->group ? ((int)b->order - a->order) : ((int)a->group - b->group);
}

void gui_sort_list(tab_t *tab)
{
    void *comp[] = {&list_comp_id_asc, &list_comp_id_desc, &list_comp_text_asc, &list_comp_text_desc};
    size_t sort_mode = tab->listbox.sort_mode - 1;

    if (!tab->listbox.length || sort_mode > RG_COUNT(comp) - 1)
        return;

    qsort((void*)tab->listbox.items, tab->listbox.length, sizeof(listbox_item_t), comp[sort_mode]);
}

void gui_resize_list(tab_t *tab, int new_size)
{
    listbox_t *list = &tab->listbox;

    if (new_size == list->length)
        return;

    // Always grow but only shrink past a certain threshold
    if (new_size >= list->capacity || list->capacity - new_size >= 20)
    {
        list->capacity = new_size + 10;
        list->items = realloc(list->items, list->capacity * sizeof(listbox_item_t));
        RG_LOGI("Resized list '%s' from %d to %d items (new capacity: %d)\n",
            tab->name, list->length, new_size, list->capacity);
    }

    for (int i = list->length; i < list->capacity; i++)
        memset(&list->items[i], 0, sizeof(listbox_item_t));

    list->length = new_size;

    if (list->cursor >= new_size)
        list->cursor = new_size ? new_size - 1 : 0;
}

void gui_scroll_list(tab_t *tab, scroll_whence_t mode, int arg)
{
    listbox_t *list = &tab->listbox;
    int list_length = list->length;
    int old_cursor = list->cursor;
    int new_cursor = RG_MAX(RG_MIN(old_cursor, list_length - 1), 0);

    if (list_length == 0)
    {
        // new_cursor = -1;
        new_cursor = 0;
    }
    else if (mode == SCROLL_SET)
    {
        new_cursor = arg;
    }
    else if (mode == SCROLL_LINE)
    {
        new_cursor += arg;
        // In line mode we wrap around
        if (new_cursor > list_length - 1)
            new_cursor = 0;
        else if (new_cursor < 0)
            new_cursor = list_length - 1;
    }
    else if (mode == SCROLL_PAGE)
    {
        new_cursor += arg * max_visible_lines(tab, NULL);
        // In page mode we stop at the edges
        if (new_cursor > list_length - 1)
            new_cursor = list_length - 1;
        else if (new_cursor < 0)
            new_cursor = 0;
    }

    // Check for invalid cursor
    if (new_cursor < 0 || new_cursor > list_length - 1)
    {
        RG_LOGW("Invalid cursor position: %d, list length: %d", new_cursor, list_length);
        new_cursor = 0; // -1;
    }

    if (list_length > 0 && list->items[new_cursor].arg)
        sprintf(tab->status[0].left, "%d / %d", (new_cursor + 1) % 10000, list_length % 10000);
    else
        strcpy(tab->status[0].left, "List empty");

    // if (new_cursor != old_cursor)
    {
        list->cursor = new_cursor;
        gui_event(TAB_SCROLL, tab);
    }
}

/* -------------------------------------------------------------------------------------- */
/* Layout                                                                                   */
/* -------------------------------------------------------------------------------------- */

/**
 * Every screen is measured from the font and the panel size rather than from constants, because
 * the supported targets run from 240x240 to 480x320 and a layout tuned for 320x240 either wastes
 * half of the big panels or overflows the small ones.
 */
static layout_t gui_layout(const tab_t *tab)
{
    layout_t l = {0};

    l.width = gui.width;
    l.height = gui.height;
    l.pad = RG_MAX(l.width / 64, 3);
    l.line_h = TEXT_RECT("ABC123", 0).height;
    l.row_h = l.line_h + 2;
    l.header_h = RG_MAX(l.line_h * 3 + l.pad * 2, 40);
    l.footer_h = l.line_h + l.pad;
    l.content_y = l.header_h + l.pad;
    l.content_h = l.height - l.content_y - l.footer_h - l.pad;

    // The preview gets its own column instead of floating over the list: covers used to sit on
    // top of the game names, which is exactly where the eye is while scrolling.
    l.has_preview = gui.show_preview != PREVIEW_MODE_NONE;
    l.preview_w = l.has_preview ? (l.width * 42) / 100 : 0;
    l.preview_x = l.width - l.pad - l.preview_w;
    l.preview_y = l.content_y;
    l.preview_h = l.content_h;

    l.list_x = l.pad;
    l.list_y = l.content_y;
    l.list_w = l.width - l.pad * 2 - SCROLLBAR_GUTTER - (l.has_preview ? l.preview_w + l.pad : 0);
    l.list_h = l.content_h;

    if (tab && tab->navpath)
    {
        l.list_y += l.line_h + 2;
        l.list_h -= l.line_h + 2;
    }

    l.list_rows = RG_MAX(l.list_h / l.row_h, 1);
    l.list_h = l.list_rows * l.row_h; // Trim the remainder so rows fill the area exactly

    return l;
}

static int max_visible_lines(const tab_t *tab, int *_line_height)
{
    layout_t l = gui_layout(tab);
    if (_line_height)
        *_line_height = l.row_h;
    return l.list_rows;
}

/* Scale an image down to fit a box, once, replacing it. Resampling on every redraw was costing
 * more than the whole rest of the frame for a cover-sized image. */
static rg_image_t *fit_image(rg_image_t *img, int max_width, int max_height)
{
    if (!img || max_width < 1 || max_height < 1)
        return img;
    if (img->width <= max_width && img->height <= max_height)
        return img;

    float scale = RG_MIN((float)max_width / img->width, (float)max_height / img->height);
    rg_image_t *scaled = rg_surface_resize(img, RG_MAX((int)(img->width * scale), 1),
                                           RG_MAX((int)(img->height * scale), 1));
    if (!scaled)
        return img;

    rg_surface_free(img);
    return scaled;
}

/* A top-down or bottom-up darkening ramp. Theme backgrounds are photos and artwork, and text on
 * top of them is only readable if we put something between the two. */
static void draw_scrim(int y, int height, int alpha_top, int alpha_bottom)
{
    if (height < 1)
        return;

    for (int i = 0; i < height; ++i)
    {
        int alpha = alpha_top + ((alpha_bottom - alpha_top) * i) / RG_MAX(height - 1, 1);
        rg_gui_fill_blend(0, y + i, gui.width, 1, C_BLACK, alpha);
    }
}

/**
 * Text that scrolls one character at a time when it does not fit, after a pause.
 *
 * Advancing by codepoints rather than pixels keeps every draw inside the box (the renderer has no
 * clip rectangle) and is UTF-8 safe by construction. Same approach as the media player, so a long
 * name behaves identically in both.
 */
static void draw_marquee(int x, int y, int width, const char *text, rg_color_t fg, rg_color_t bg, bool active)
{
    if (!text || !*text || width <= 0)
        return;

    if (!active || TEXT_RECT(text, 0).width <= width)
    {
        rg_gui_draw_text(x, y, width, text, fg, bg, RG_TEXT_ALIGN_LEFT);
        return;
    }

    size_t offsets[96];
    int steps = 0;
    offsets[0] = 0;

    for (const char *p = text; *p && steps < (int)RG_COUNT(offsets) - 1;)
    {
        const char *next = p;
        rg_utf8_decode(&next);
        if (next == p)
            break;
        offsets[++steps] = (size_t)(next - text);
        if (TEXT_RECT(next, 0).width <= width)
            break; // The remainder now fits, this is the last useful position
        p = next;
    }

    const int64_t hold_ms = 1000, step_ms = 130;
    int64_t travel_ms = (int64_t)steps * step_ms;
    int64_t cycle = hold_ms * 2 + travel_ms * 2;
    int64_t phase = cycle > 0 ? ((rg_system_timer() / 1000) % cycle) : 0;
    int step;

    if (phase < hold_ms)
        step = 0;
    else if (phase < hold_ms + travel_ms)
        step = (int)((phase - hold_ms) / step_ms);
    else if (phase < hold_ms * 2 + travel_ms)
        step = steps;
    else
        step = steps - (int)((phase - hold_ms * 2 - travel_ms) / step_ms);

    step = RG_MIN(RG_MAX(step, 0), steps);
    rg_gui_draw_text(x, y, width, text + offsets[step], fg, bg, RG_TEXT_ALIGN_LEFT);
}

/**
 * True while something on screen is moving, so the main loop knows it has to keep redrawing.
 *
 * It is deliberately narrow: only the selected row's name, and only when it is too long to fit.
 * Redrawing the whole launcher ten times a second for a name that already fits would burn CPU
 * (and battery) for no visible difference.
 */
bool gui_has_animation(void)
{
    if (!gui.browse)
        return false;

    tab_t *tab = gui_get_current_tab();
    if (!tab || tab->listbox.length < 1)
        return false;

    const listbox_item_t *item = &tab->listbox.items[RG_MIN(RG_MAX(tab->listbox.cursor, 0), tab->listbox.length - 1)];
    layout_t l = gui_layout(tab);

    return TEXT_RECT(item->text, 0).width > l.list_w - l.pad * 2 - ROW_TEXT_INSET;
}

/* -------------------------------------------------------------------------------------- */
/* Drawing                                                                                  */
/* -------------------------------------------------------------------------------------- */

void gui_redraw(void)
{
    rg_display_sync(true);
    rg_gui_set_surface(gui.surface);

    tab_t *tab = gui_get_current_tab();
    if (!tab)
    {
        RG_LOGW("No tab to redraw...");
    }
    else if (gui.browse)
    {
        gui_draw_background(tab, 3);
        gui_draw_header(tab, 0);
        gui_draw_status(tab);
        gui_draw_list(tab);
        gui_draw_preview(tab);
        gui_draw_footer(tab);
    }
    else
    {
        layout_t l = gui_layout(tab);
        gui_draw_background(tab, 0);
        // Scrims top and bottom: the artwork is full brightness on this screen, and the status
        // icons and hint line have to stay readable over whatever the theme puts behind them.
        draw_scrim(0, l.header_h, 160, 0);
        draw_scrim(l.height - l.footer_h * 2, l.footer_h * 2, 0, 175);
        gui_draw_header(tab, 0);
        gui_draw_tab_indicator();
        rg_gui_draw_icons();
    }

    rg_gui_set_surface(NULL);
    rg_display_submit(gui.surface, 0);
}

void gui_draw_background(tab_t *tab, int shade)
{
    // We can't losslessly change shade, must reload!
    if (tab->background && tab->background_shade > 0 && tab->background_shade != shade)
    {
        rg_surface_free(tab->background);
        tab->background = NULL;
    }

    if (!tab->background)
    {
        tab->background = gui_get_image("background", tab->name); // Try background_<tabname>.png
        if (!tab->background)
            tab->background = gui_get_image("background", NULL); // Fallback to a background.png
        tab->background_shade = 0;
        if (tab->background && (tab->background->width != gui.width || tab->background->height != gui.height))
        {
            rg_image_t *temp = rg_surface_resize(tab->background, gui.width, gui.height);
            if (temp)
            {
                rg_surface_free(tab->background);
                tab->background = temp;
            }
        }
    }

    if (tab->background && tab->background_shade != shade && shade > 0)
    {
        rg_image_t *img = tab->background;
        for (int y = 0; y < img->height; ++y)
        {
            uint16_t *line = img->data + y * img->stride;
            for (int x = 0; x < img->width; ++x)
            {
                int pixel = line[x];
                int r = ((pixel >> 11) & 0x1F) / shade;
                int g = ((pixel >> 5) & 0x3F) / shade;
                int b = ((pixel) & 0x1F) / shade;
                line[x] = ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | ((b & 0x1F) << 0);
            }
        }
        tab->background_shade = shade;
    }

    if (tab->background)
        rg_gui_draw_image(0, 0, gui.width, gui.height, false, tab->background);
    else
        rg_gui_draw_rect(0, 0, gui.width, gui.height, 0, 0, gui.theme->background);

    // Without a theme image the flat background is very plain, so put a soft accent glow in the
    // corner. It costs one gradient and gives the default look some depth.
    if (!tab->background)
    {
        const rg_gui_palette_t *pal = rg_gui_get_palette();
        rg_gui_draw_gradient(0, 0, gui.width, gui.height / 2, rg_gui_blend_color(gui.theme->background, pal->accent, 40),
                             gui.theme->background, false, 255);
    }
}

/**
 * The header: system logo, name, and (in the browser) the item counter and status text.
 *
 * In the carousel it is a centered card with the tab's banner artwork, because that screen is
 * about picking a system and the artwork is the whole point. In the browser it is a slim band, so
 * the list gets the space instead.
 */
void gui_draw_header(tab_t *tab, int offset)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    layout_t l = gui_layout(tab);
    int logo_box = l.header_h - l.pad * 2;

    if (!tab->logo)
        tab->logo = fit_image(gui_get_image("logo", tab->name), logo_box, logo_box);

    if (gui.browse)
    {
        // Slim band with a hairline, then logo, name and status
        rg_gui_fill_blend(0, 0, l.width, l.header_h, pal->background, 165);
        rg_gui_fill_blend(0, l.header_h - 1, l.width, 1, pal->accent, 150);

        int x = l.pad;

        if (tab->logo)
        {
            rg_gui_draw_image(x, (l.header_h - tab->logo->height) / 2, 0, 0, false, tab->logo);
            x += tab->logo->width + l.pad;
        }

        // A quarter of the width is left free on the right for the battery/clock cluster, so a long
        // system name never runs underneath it.
        rg_gui_draw_text(x, l.pad, l.width - x - l.pad - l.width / 4, tab->desc, gui.theme->foreground,
                         C_TRANSPARENT, RG_TEXT_BIGGER | RG_TEXT_ALIGN_LEFT);
        return;
    }

    // Carousel: a card holding the logo and the banner, centered a little above the middle so the
    // tab indicator and the hint line below it have room to breathe.
    int card_w = l.width - l.pad * 4;
    int card_h = logo_box + l.pad * 3 + l.line_h;
    int card_x = (l.width - card_w) / 2;
    int card_y = (l.height - card_h) / 2 - l.line_h + offset;

    // Fitted to the space it will actually occupy inside the card, next to the logo
    if (!tab->banner)
        tab->banner = fit_image(gui_get_image("banner", tab->name), card_w - l.pad * 6 - logo_box, l.line_h * 2 + 6);

    rg_gui_draw_shadow(card_x, card_y, card_w, card_h, 8, 3);
    rg_gui_draw_panel(card_x, card_y, card_w, card_h, 8, pal->surface, pal->divider, 175);
    // Accent edge along the top of the card, the same cue the dialogs use for their header chip
    rg_gui_fill_blend(card_x + 8, card_y + 1, card_w - 16, 1, pal->accent, 190);

    int content_x = card_x + l.pad * 2;
    int content_w = card_w - l.pad * 4;

    if (tab->logo)
    {
        rg_gui_draw_image(content_x, card_y + l.pad + (logo_box - tab->logo->height) / 2, 0, 0, false, tab->logo);
        content_x += tab->logo->width + l.pad * 2;
        content_w -= tab->logo->width + l.pad * 2;
    }

    if (tab->banner)
        rg_gui_draw_image(content_x, card_y + l.pad + (logo_box - tab->banner->height) / 2, 0, 0, false, tab->banner);
    else
        rg_gui_draw_text(content_x, card_y + l.pad + (logo_box - l.line_h * 2) / 2, content_w, tab->desc,
                         gui.theme->foreground, C_TRANSPARENT, RG_TEXT_BIGGER | RG_TEXT_ALIGN_LEFT);

    // Status line inside the card: whatever the tab wants to say (it is where "Loading...", the
    // media player's now-playing line and error text end up), otherwise how many entries it holds.
    // The cursor position that status[0] carries belongs to the browser, not here.
    char count[24] = {0};
    const char *txt_left = tab->status[1].left;
    const char *txt_right = tab->status[tab->status[1].right[0] ? 1 : 0].right;
    int status_y = card_y + card_h - l.pad - l.line_h;

    if (!txt_left[0] && tab->initialized)
    {
        snprintf(count, sizeof(count), "%d %s", tab->listbox.length % 100000, _("items"));
        txt_left = count;
    }

    // Drawn only when there is something to say: an empty rule under an empty line looks broken
    if ((txt_left && *txt_left) || (txt_right && *txt_right))
    {
        rg_gui_fill_blend(card_x + l.pad * 2, status_y - l.pad / 2, card_w - l.pad * 4, 1, pal->divider, 200);
        if (txt_left && *txt_left)
            rg_gui_draw_text(card_x + l.pad * 2, status_y, card_w - l.pad * 4, txt_left, pal->text_dim, C_TRANSPARENT,
                             RG_TEXT_ALIGN_LEFT);
        if (txt_right && *txt_right)
            rg_gui_draw_text(card_x + l.pad * 2, status_y, card_w - l.pad * 4, txt_right, pal->text_dim, C_TRANSPARENT,
                             RG_TEXT_ALIGN_RIGHT);
    }
}

/* Status: the counter and messages, plus the battery/network/clock cluster. */
void gui_draw_status(tab_t *tab)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    layout_t l = gui_layout(tab);
    // Only the tab's own message goes here ("Loading...", the media player's now-playing line).
    // The item counter lives in the hint bar at the bottom, and printing it twice just made the
    // header look busy.
    const char *txt_left = tab->status[1].left;
    const char *txt_right = tab->status[tab->status[1].right[0] ? 1 : 0].right;
    int x = l.pad + (tab->logo ? tab->logo->width + l.pad : 0);
    int y = l.pad + l.line_h * 2;

    // The right-hand status text would run under the icon cluster, so it is placed on the left of
    // the second line and the icons keep the corner to themselves.
    if (txt_left && *txt_left)
        rg_gui_draw_text(x, y, 0, txt_left, pal->text_dim, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    if (txt_right && *txt_right)
    {
        int left_width = (txt_left && *txt_left) ? TEXT_RECT(txt_left, 0).width + l.pad * 2 : 0;
        // A chip, so a warning like "No cover" reads as a badge rather than as more body text
        int chip_w = TEXT_RECT(txt_right, 0).width + l.pad * 2;
        rg_gui_draw_panel(x + left_width, y - 1, chip_w, l.line_h + 2, 3, pal->surface_alt, C_NONE, 220);
        rg_gui_draw_text(x + left_width + l.pad, y, 0, txt_right, pal->accent, C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);
    }

    rg_gui_draw_icons();
}

void gui_draw_list(tab_t *tab)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    const listbox_t *list = &tab->listbox;
    layout_t l = gui_layout(tab);
    int line_offset;

    if (tab->navpath)
    {
        // Breadcrumb chip for the folder we are inside
        int width = RG_MIN(TEXT_RECT(tab->navpath, 0).width + l.pad * 3, l.list_w);
        rg_gui_draw_panel(l.list_x, l.content_y, width, l.line_h + 2, 3, pal->surface_alt, C_NONE, 200);
        rg_gui_draw_text(l.list_x + l.pad, l.content_y + 1, width - l.pad * 2, tab->navpath, pal->text, C_TRANSPARENT,
                         RG_TEXT_ALIGN_LEFT);
    }

    if (gui.scroll_mode == SCROLL_MODE_PAGING)
        line_offset = (list->cursor / l.list_rows) * l.list_rows;
    else // (gui.scroll_mode == SCROLL_MODE_CENTER)
        line_offset = list->cursor - (l.list_rows / 2);

    // Keep the window inside the list so the last page is full instead of half empty
    if (line_offset > list->length - l.list_rows)
        line_offset = list->length - l.list_rows;
    if (line_offset < 0)
        line_offset = 0;

    if (list->length == 0)
    {
        rg_gui_draw_text(l.list_x, l.list_y + l.list_h / 2 - l.line_h, l.list_w, _("No files"), pal->text_dim,
                         C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
        return;
    }

    for (int i = 0; i < l.list_rows; i++)
    {
        int idx = line_offset + i;
        int y = l.list_y + i * l.row_h;
        bool selected = idx == list->cursor;
        rg_color_t fg = selected ? gui.theme->list.selected_fg : gui.theme->list.standard_fg;
        rg_color_t bg = selected ? gui.theme->list.selected_bg : gui.theme->list.standard_bg;

        if (idx < 0 || idx >= list->length)
            continue;

        if (selected)
        {
            // Selection pill with a leading accent bar. A theme that sets an opaque
            // list_selected_bg gets its own color for the pill, otherwise we tint the accent.
            rg_color_t pill = (bg == C_TRANSPARENT) ? pal->accent_dim : bg;
            int alpha = (bg == C_TRANSPARENT) ? 210 : 255;
            rg_gui_draw_panel(l.list_x, y, l.list_w, l.row_h, RG_MIN(4, l.row_h / 2), pill, C_NONE, alpha);
            rg_gui_draw_panel(l.list_x, y + 1, 3, l.row_h - 2, 1, pal->accent, C_NONE, 255);
        }
        else if (bg != C_TRANSPARENT)
        {
            rg_gui_draw_panel(l.list_x, y, l.list_w, l.row_h, RG_MIN(4, l.row_h / 2), bg, C_NONE, 255);
        }

        draw_marquee(l.list_x + ROW_TEXT_INSET, y + (l.row_h - l.line_h) / 2, l.list_w - ROW_TEXT_INSET - l.pad,
                     list->items[idx].text, fg, C_TRANSPARENT, selected);
    }

    rg_gui_draw_scrollbar(l.list_x + l.list_w + 1, l.list_y, l.list_h, l.list_rows, list->length, line_offset);
}

/* Cover art or save-state screenshot, in a card of its own on the right of the list. */
void gui_draw_preview(tab_t *tab)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    layout_t l = gui_layout(tab);

    if (!l.has_preview)
        return;

    int inner_w = l.preview_w - 6;
    int inner_h = l.preview_h - 6;
    int img_w = tab->preview ? RG_MIN(tab->preview->width, inner_w) : inner_w;
    int img_h = tab->preview ? RG_MIN(tab->preview->height, inner_h) : (inner_w * 3) / 4;
    int card_w = img_w + 6;
    int card_h = img_h + 6;
    int card_x = l.preview_x + (l.preview_w - card_w) / 2;
    int card_y = l.preview_y + (l.preview_h - card_h) / 2;

    rg_gui_draw_shadow(card_x, card_y, card_w, card_h, 5, 3);
    rg_gui_draw_panel(card_x, card_y, card_w, card_h, 5, pal->surface, pal->divider, tab->preview ? 235 : 150);

    if (tab->preview)
    {
        rg_gui_draw_image(card_x + 3, card_y + 3, img_w, img_h, false, tab->preview);
    }
    else
    {
        // Placeholder: the system's own logo, dimmed, instead of an empty hole in the layout
        if (tab->logo)
        {
            int lx = card_x + (card_w - tab->logo->width) / 2;
            int ly = card_y + (card_h - tab->logo->height) / 2 - l.line_h / 2;
            rg_gui_draw_image(lx, ly, 0, 0, false, tab->logo);
            rg_gui_dim_area(lx, ly, tab->logo->width, tab->logo->height, 110);
        }
        rg_gui_draw_text(card_x, card_y + card_h - l.line_h - 3, card_w, _("No cover"), pal->text_dim, C_TRANSPARENT,
                         RG_TEXT_ALIGN_CENTER);
    }
}

/* Hint bar. Which buttons do what is the one thing a launcher should never make you guess. */
void gui_draw_footer(tab_t *tab)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    layout_t l = gui_layout(tab);
    int y = l.height - l.footer_h;
    char counter[24] = {0};

    rg_gui_fill_blend(0, y, l.width, l.footer_h, pal->background, 165);
    rg_gui_fill_blend(0, y, l.width, 1, pal->divider, 220);

    rg_gui_draw_text(l.pad, y + l.pad / 2, l.width / 2, _("A Launch   B Back   MENU Info"), pal->text_dim,
                     C_TRANSPARENT, RG_TEXT_ALIGN_LEFT);

    if (tab->listbox.length > 0)
    {
        snprintf(counter, sizeof(counter), "%d / %d", (tab->listbox.cursor + 1) % 100000,
                 tab->listbox.length % 100000);
        rg_gui_draw_text(l.width / 2, y + l.pad / 2, l.width / 2 - l.pad, counter, pal->text, C_TRANSPARENT,
                         RG_TEXT_ALIGN_RIGHT);
    }
}

/* Which system you are on, as pills at the bottom of the carousel. */
void gui_draw_tab_indicator(void)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    layout_t l = gui_layout(gui_get_current_tab());
    int dot = RG_MAX(l.pad, 4);
    int gap = RG_MAX(l.pad - 1, 3);
    int active_w = dot * 3;
    int count = 0, active_index = 0;

    for (size_t i = 0; i < gui.tabs_count; ++i)
    {
        if (!gui.tabs[i]->enabled)
            continue;
        if ((int)i == gui.selected_tab)
            active_index = count;
        count++;
    }

    if (count < 1)
        return;

    int total = (count - 1) * (dot + gap) + active_w;
    int x = (l.width - total) / 2;
    int y = l.height - l.footer_h - dot;

    for (int i = 0, drawn = 0; i < (int)gui.tabs_count; ++i)
    {
        if (!gui.tabs[i]->enabled)
            continue;

        bool active = drawn == active_index;
        int width = active ? active_w : dot;
        rg_gui_draw_panel(x, y, width, dot, dot / 2, active ? pal->accent : pal->text_dim, C_NONE, active ? 255 : 150);
        x += width + gap;
        drawn++;
    }

    rg_gui_draw_text(0, l.height - l.line_h - 2, l.width, _("A Open   LEFT RIGHT Systems   MENU Info"), pal->text_dim,
                     C_TRANSPARENT, RG_TEXT_ALIGN_CENTER);
}

void gui_set_preview(tab_t *tab, rg_image_t *preview)
{
    if (!tab)
        return;

    if (tab->preview)
        rg_surface_free(tab->preview);

    // Scale to the card once, here, rather than resampling it on every redraw
    if (preview)
    {
        layout_t l = gui_layout(tab);
        preview = fit_image(preview, RG_MAX(l.preview_w - 6, 8), RG_MAX(l.preview_h - 6, 8));
    }

    tab->preview = preview;
}

void gui_load_preview(tab_t *tab)
{
    listbox_item_t *item = gui_get_selected_item(tab);
    bool show_missing_cover = false;
    uint32_t order;

    gui_set_preview(tab, NULL);

    if (!item || !item->arg || gui.low_memory_mode)
        return;

    switch (gui.show_preview)
    {
        case PREVIEW_MODE_COVER_SAVE:
            show_missing_cover = true;
            order = 0x4123;
            break;
        case PREVIEW_MODE_SAVE_COVER:
            show_missing_cover = true;
            order = 0x1234;
            break;
        case PREVIEW_MODE_COVER_ONLY:
            show_missing_cover = true;
            order = 0x0123;
            break;
        case PREVIEW_MODE_SAVE_ONLY:
            show_missing_cover = false;
            order = 0x0004;
            break;
        default:
            show_missing_cover = false;
            order = 0x0000;
    }

    retro_file_t *file = item->arg;
    retro_app_t *app = file->app;
    uint32_t errors = 0;

    while (order && !tab->preview)
    {
        char path[RG_PATH_MAX + 1];
        size_t path_len = 0;
        int type = order & 0xF;

        order >>= 4;

        // Give up on any button press to improve responsiveness
        if ((gui.joystick |= rg_input_read_gamepad()))
            break;

        if (file->missing_cover & (1 << type))
            continue;

        if (type == 0x1 && app->use_crc_covers && application_get_file_crc32(file)) // Game cover (old format)
            path_len = snprintf(path, RG_PATH_MAX, "%s/%X/%08X.art", app->paths.covers, (int)(file->checksum >> 28), (int)file->checksum);
        else if (type == 0x2 && app->use_crc_covers && application_get_file_crc32(file)) // Game cover (png)
            path_len = snprintf(path, RG_PATH_MAX, "%s/%X/%08X.png", app->paths.covers, (int)(file->checksum >> 28), (int)file->checksum);
        else if (type == 0x3) // Game cover (based on filename)
        {
            path_len = snprintf(path, RG_PATH_MAX, "%s/%s", app->paths.covers, file->name);
            if (path_len < RG_PATH_MAX - 3) // Don't bother if we already have an overflow
                strcpy(path + path_len - strlen(rg_extension(file->name) ?: ""), "png");
        }
        else if (type == 0x4 && file->saves > 0) // Save state screenshot (png)
        {
            snprintf(path, RG_PATH_MAX, "%s/%s", file->folder, file->name);
            uint8_t last_used_slot = rg_emu_get_last_used_slot(path);
            if (last_used_slot != 0xFF)
            {
                char *preview = rg_emu_get_path(RG_PATH_SCREENSHOT + last_used_slot, path);
                path_len = snprintf(path, RG_PATH_MAX, "%s", preview);
                free(preview);
            }
        }

        if (path_len > 0 && path_len < RG_PATH_MAX)
        {
            RG_LOGD("Looking for %s", path);
            gui_set_preview(tab, rg_surface_load_image_file(path, 0));
            // if (!tab->preview && rg_storage_exists(path))
            //     errors++;
        }

        file->missing_cover |= (tab->preview ? 0 : 1) << type;
    }

    if (!tab->preview && file->checksum && (show_missing_cover || errors))
    {
        RG_LOGD("No image found for '%s'\n", file->name);
        gui_set_status(tab, NULL, errors ? "Bad cover" : "No cover");
        // gui_draw_status(tab);
        // tab->preview = gui_get_image("cover", file->app);
    }
}
