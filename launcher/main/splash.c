/**
 * Animated boot screen.
 *
 * It draws into its own surface with the regular rg_gui primitives, which means the whole scene is
 * composited in RAM and sent as finished frames: nothing here talks to the panel directly, and
 * nothing depends on the launcher having been initialised yet.
 *
 * The scene is deliberately built from shapes rather than from a bitmap. A 320x240 image would
 * cost ~150 KB of flash, would have to exist in several sizes for the 240x240 / 320x240 / 480x320
 * targets, and could not animate. Everything below is placed relative to the panel size instead.
 */

#include <rg_system.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"
#include "splash.h"

#define SETTING_BOOT_ANIMATION "BootAnimation"

/* Timeline, in milliseconds from the first frame. Paced so each beat is readable on its own: the
 * scene settles, the mark drops and switches on, the wordmark comes up, the highlight crosses it,
 * and only then does it hand over. */
#define T_LOGO_IN    280  // the device mark starts dropping in
#define T_LOGO_DONE  1040 // ...and has landed
#define T_TEXT_IN    1020 // wordmark starts to fade up
#define T_TEXT_DONE  1620
#define T_SHINE      1780 // highlight sweeps across the wordmark
#define T_SHINE_DONE 2560
#define T_HOLD       2960 // everything is on screen, hold it for a beat
#define T_FADE_DONE  3380 // faded out, launcher takes over

#define FRAME_MS   30
#define STAR_COUNT 40

/* rg_gui reads a plain negative y as "measured from the bottom of the screen", which is not what
 * the mark sliding in from above the top edge wants. Asking for a position relative to the top
 * keeps a negative offset meaning what it says; the primitives clip it. Note that lines and discs
 * take absolute coordinates and must not be wrapped. */
#define TOP_Y(y) (RG_GUI_TOP + (y))

static float clampf(float value, float min, float max)
{
    return value < min ? min : (value > max ? max : value);
}

/* 0..1 progress between two timeline marks. */
static float phase(int64_t now_ms, int from_ms, int to_ms)
{
    if (now_ms <= from_ms)
        return 0.0f;
    if (now_ms >= to_ms)
        return 1.0f;
    return (float)(now_ms - from_ms) / (float)(to_ms - from_ms);
}

static float ease_out_cubic(float t)
{
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

/* Overshoots slightly past its target and settles back: the device mark lands with a little
 * weight instead of just arriving. */
static float ease_out_back(float t)
{
    const float c = 1.9f;
    float inv = t - 1.0f;
    return 1.0f + (c + 1.0f) * inv * inv * inv + c * inv * inv;
}

/* -------------------------------------------------------------------------------------- */
/* Scene                                                                                    */
/* -------------------------------------------------------------------------------------- */

static struct
{
    int16_t x, y;
    uint8_t size, twinkle;
} stars[STAR_COUNT];

static void stars_init(int width, int horizon)
{
    // Fixed seed: the sky is the same every boot, which is what a logo screen should look like
    uint32_t seed = 0x51ED2A9B;

    for (int i = 0; i < STAR_COUNT; ++i)
    {
        seed = seed * 1103515245 + 12345;
        stars[i].x = (int16_t)((seed >> 16) % (uint32_t)RG_MAX(width, 1));
        seed = seed * 1103515245 + 12345;
        stars[i].y = (int16_t)((seed >> 16) % (uint32_t)RG_MAX(horizon - 2, 1));
        seed = seed * 1103515245 + 12345;
        stars[i].size = ((seed >> 16) % 100) > 80 ? 2 : 1;
        seed = seed * 1103515245 + 12345;
        stars[i].twinkle = (seed >> 16) & 0xFF;
    }
}

static void draw_sky(int width, int horizon, int height)
{
    // Dusk gradient above the horizon, near-black below it
    rg_gui_draw_gradient(0, 0, width, horizon, C_RGB(10, 8, 26), C_RGB(120, 26, 96), false, 255);
    rg_gui_draw_gradient(0, horizon, width, height - horizon, C_RGB(28, 8, 40), C_RGB(6, 4, 14), false, 255);
}

static void draw_stars(int64_t now_ms, float alpha)
{
    for (int i = 0; i < STAR_COUNT; ++i)
    {
        // Each star breathes at its own rate, from its own offset
        float wave = sinf((float)(now_ms + stars[i].twinkle * 12) / 420.0f + (float)i);
        int a = (int)((0.45f + 0.55f * wave * wave) * 210.0f * alpha);
        rg_gui_fill_blend(stars[i].x, stars[i].y, stars[i].size, stars[i].size, C_RGB(255, 244, 232), a);
    }
}

/* The sun: a disc with a vertical gradient, cut by bands that get thicker toward the bottom. */
static void draw_sun(int center_x, int center_y, int radius, float alpha)
{
    if (radius < 4)
        return;

    for (int dy = -radius; dy <= radius; ++dy)
    {
        int band = dy + radius;

        // Below the middle the disc breaks up into stripes; the gaps widen as they go down
        if (dy > radius / 8)
        {
            int period = RG_MAX(radius / 5 - dy / RG_MAX(radius / 3, 1), 3);
            if ((band % period) < RG_MAX(period / 3, 1))
                continue;
        }

        int dx = (int)(sqrtf((float)(radius * radius - dy * dy)) + 0.5f);
        rg_color_t color = rg_gui_blend_color(C_RGB(255, 226, 130), C_RGB(255, 72, 120),
                                             (band * 255) / RG_MAX(radius * 2, 1));
        rg_gui_fill_blend(center_x - dx, center_y + dy, dx * 2 + 1, 1, color, (int)(255 * alpha));
    }
}

/* Perspective grid running to a vanishing point on the horizon, scrolling toward the viewer. */
static void draw_grid(int width, int height, int horizon, int64_t now_ms, float alpha)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    rg_color_t color = rg_gui_blend_color(pal->accent, C_RGB(255, 80, 190), 110);
    int depth = height - horizon;
    int center_x = width / 2;

    if (depth < 8)
        return;

    // Verticals: evenly spaced at the bottom edge, all converging on the vanishing point
    for (int i = -7; i <= 7; ++i)
    {
        int bottom_x = center_x + (i * width) / 5;
        rg_gui_draw_line(center_x, horizon, bottom_x, height - 1, color, (int)(90 * alpha));
    }

    // Horizontals: spacing grows with the square of the distance from the horizon, and the whole
    // set is offset over time so the ground appears to move underneath us.
    float scroll = fmodf((float)now_ms / 900.0f, 1.0f);
    for (int i = 0; i < 9; ++i)
    {
        float t = ((float)i + scroll) / 9.0f;
        int y = horizon + (int)(t * t * depth);
        if (y >= horizon && y < height)
            rg_gui_draw_line(0, y, width - 1, y, color, (int)(clampf(t * 1.6f, 0.15f, 1.0f) * 130 * alpha));
    }

    // A bright line on the horizon itself, plus a bloom above it
    rg_gui_draw_line(0, horizon, width - 1, horizon, C_RGB(255, 200, 230), (int)(210 * alpha));
    rg_gui_fill_blend(0, horizon - 2, width, 2, C_RGB(255, 120, 190), (int)(60 * alpha));
}

/**
 * The mark: a stylised handheld, drawn from panels and discs.
 *
 * `power` is 0..1 for the screen lighting up, and is driven separately from the drop-in animation
 * in the caller: that is what makes the console look like it switches on after it lands.
 */
static void draw_device(int center_x, int center_y, int body_w, int body_h, float power)
{
    const rg_gui_palette_t *pal = rg_gui_get_palette();
    int x = center_x - body_w / 2;
    int y = center_y - body_h / 2;
    int radius = RG_MAX(body_h / 5, 4);
    int inset = RG_MAX(body_w / 16, 3);

    rg_gui_draw_shadow(x, TOP_Y(y), body_w, body_h, radius, 3);
    rg_gui_draw_panel(x, TOP_Y(y), body_w, body_h, radius, C_RGB(30, 32, 46), C_RGB(70, 76, 100), 255);
    // Specular edge along the top, so the plastic reads as rounded
    rg_gui_fill_blend(x + radius, TOP_Y(y + 1), body_w - radius * 2, 1, C_RGB(150, 160, 190), 170);

    // Screen
    int screen_w = (body_w * 42) / 100;
    int screen_h = (body_h * 52) / 100;
    int screen_x = x + (body_w - screen_w) / 2;
    int screen_y = y + inset + 1;

    rg_gui_draw_panel(screen_x, TOP_Y(screen_y), screen_w, screen_h, 3, C_RGB(10, 12, 20), C_RGB(60, 66, 88), 255);

    if (power > 0.0f)
    {
        // The screen fills with an accent gradient and gets scanlines, then spills a little light
        // onto the shell around it.
        int fill_h = RG_MAX((int)(screen_h * power) - 2, 0);
        if (fill_h > 0)
        {
            int fill_y = screen_y + 1 + (screen_h - 2 - fill_h) / 2;
            rg_gui_draw_gradient(screen_x + 1, TOP_Y(fill_y), screen_w - 2, fill_h, pal->accent,
                                 C_RGB(255, 90, 170), false, 255);
            for (int sy = fill_y; sy < fill_y + fill_h; sy += 2)
                rg_gui_fill_blend(screen_x + 1, TOP_Y(sy), screen_w - 2, 1, C_BLACK, 60);
        }
        rg_gui_fill_blend(screen_x - 1, TOP_Y(screen_y - 1), screen_w + 2, screen_h + 2, pal->accent,
                          (int)(40 * power));
    }

    // D-pad, as a plus made of two bars
    int pad_size = RG_MAX(body_h / 3, 9);
    int bar = RG_MAX(pad_size / 3, 3);
    int pad_x = x + inset + pad_size / 2;
    int pad_y = y + body_h - inset - pad_size / 2 - bar / 2;

    rg_gui_draw_panel(pad_x - pad_size / 2, TOP_Y(pad_y - bar / 2), pad_size, bar, 1, C_RGB(88, 94, 118), C_NONE,
                      255);
    rg_gui_draw_panel(pad_x - bar / 2, TOP_Y(pad_y - pad_size / 2), bar, pad_size, 1, C_RGB(88, 94, 118), C_NONE,
                      255);

    // Two face buttons, offset from each other the way a real pad has them
    int button_r = RG_MAX(bar / 2 + 1, 3);
    int button_x = x + body_w - inset - button_r;
    int button_y = pad_y;

    rg_gui_draw_disc(button_x, button_y - button_r, button_r, C_RGB(255, 96, 150), 255);
    rg_gui_draw_disc(button_x - button_r * 2 - 2, button_y + button_r, button_r, pal->accent, 255);
}

/* -------------------------------------------------------------------------------------- */
/* Entry point                                                                              */
/* -------------------------------------------------------------------------------------- */

#define SETTING_SPLASH_PENDING "BootAnimationPending"

bool splash_enabled(void)
{
    return rg_settings_get_number(NS_APP, SETTING_BOOT_ANIMATION, 1) != 0;
}

/**
 * Ask for the animation on the next boot.
 *
 * rg_system_init() only reports a cold boot when the reset reason is not ESP_RST_SW, so a reboot we
 * trigger ourselves would otherwise skip the animation - which is the one thing the user asked for
 * when they chose Reboot.
 */
void splash_request(void)
{
    rg_settings_set_number(NS_APP, SETTING_SPLASH_PENDING, 1);
    rg_settings_commit();
}

void splash_set_enabled(bool enabled)
{
    rg_settings_set_number(NS_APP, SETTING_BOOT_ANIMATION, enabled ? 1 : 0);
}

void splash_show(bool cold_boot)
{
    bool requested = rg_settings_get_number(NS_APP, SETTING_SPLASH_PENDING, 0) != 0;

    if (requested)
    {
        // One-shot: consume it now so a crash mid-animation cannot make it loop
        rg_settings_set_number(NS_APP, SETTING_SPLASH_PENDING, 0);
        rg_settings_commit();
    }

    // Only on a cold boot or an explicit reboot: coming back from an emulator, the user is waiting
    // to get to their list, not to watch a logo.
    if ((!cold_boot && !requested) || !splash_enabled())
        return;

    // Anything held down at boot skips it, so it can never get in the way
    if (rg_input_read_gamepad())
        return;

    int width = rg_display_get_width();
    int height = rg_display_get_height();
    rg_surface_t *surface = rg_surface_create(width, height, RG_PIXEL_565_LE, MEM_SLOW);

    if (!surface)
    {
        RG_LOGW("Not enough memory for the boot animation, skipping it");
        return;
    }

    const rg_gui_palette_t *pal = rg_gui_get_palette();
    const rg_app_t *app = rg_system_get_app();
    int horizon = (height * 62) / 100;
    int sun_radius = RG_MAX(height / 7, 12);
    int device_w = RG_MIN((width * 34) / 100, 116);
    int device_h = RG_MAX((device_w * 62) / 100, 32);
    int device_rest_y = horizon - device_h / 3;
    int line_h = rg_gui_get_font_height() + 2;
    char status[64];

    snprintf(status, sizeof(status), "%s %s   %s   %s", RG_PROJECT_NAME, app->version, RG_TARGET_NAME,
             rg_storage_ready() ? "SD OK" : _("No SD card"));

    stars_init(width, horizon);

    int64_t start = rg_system_timer();
    int64_t now_ms = 0;

    while (now_ms < T_FADE_DONE)
    {
        now_ms = (rg_system_timer() - start) / 1000;

        // Any key skips straight to the fade, so the animation is never something to sit through
        if (rg_input_read_gamepad() && now_ms < T_HOLD)
        {
            start = rg_system_timer() - T_HOLD * 1000;
            now_ms = T_HOLD;
        }

        rg_display_sync(true);
        rg_gui_set_surface(surface);

        float scene_in = ease_out_cubic(phase(now_ms, 0, T_LOGO_DONE));
        float landing = ease_out_back(phase(now_ms, T_LOGO_IN, T_LOGO_DONE));
        float power = ease_out_cubic(phase(now_ms, T_LOGO_DONE - 160, T_LOGO_DONE + 420));
        float text_in = ease_out_cubic(phase(now_ms, T_TEXT_IN, T_TEXT_DONE));

        draw_sky(width, horizon, height);
        draw_stars(now_ms, scene_in);
        draw_sun(width / 2, horizon - sun_radius / 3, sun_radius, scene_in);
        draw_grid(width, height, horizon, now_ms, scene_in);

        // Drops in from above the screen and settles at its resting position
        int device_y = (int)(-device_h + (device_rest_y + device_h) * landing);
        draw_device(width / 2, device_y, device_w, device_h, power);

        // Wordmark: the color fades up from the sky tone rather than the text sliding around,
        // because glyphs are the one thing here we cannot move sub-pixel.
        int text_y = device_rest_y + device_h / 2 + line_h;
        rg_color_t text_color = rg_gui_blend_color(C_RGB(70, 40, 96), C_RGB(255, 255, 255), (int)(text_in * 255));
        rg_rect_t mark = rg_gui_draw_text(0, text_y, width, RG_PROJECT_NAME, text_color, C_TRANSPARENT,
                                          RG_TEXT_BIGGER | RG_TEXT_ALIGN_CENTER);

        if (text_in >= 1.0f)
        {
            // Specular sweep across the wordmark
            float sweep = phase(now_ms, T_SHINE, T_SHINE_DONE);
            if (sweep > 0.0f && sweep < 1.0f)
            {
                int band = RG_MAX(width / 10, 8);
                int center = (int)((width + band * 2) * sweep) - band;
                for (int i = -band; i <= band; ++i)
                {
                    // A negative x would be read as an offset from the right edge, so columns that
                    // fall outside the screen are skipped rather than wrapped
                    int column = center + i;
                    if (column < 0 || column >= width)
                        continue;
                    rg_gui_fill_blend(column, mark.top, 1, mark.height, C_WHITE,
                                      (int)(80.0f * (1.0f - fabsf((float)i / band))));
                }
            }

            // Underline that grows out from the middle, tying the mark to the accent color
            int rule_w = (int)(mark.width * 0.44f * clampf((float)(now_ms - T_TEXT_DONE) / 420.0f, 0.0f, 1.0f));
            if (rule_w > 1)
                rg_gui_draw_panel((width - rule_w) / 2, mark.top + mark.height + 2, rule_w, 2, 1, pal->accent, C_NONE,
                                  255);
        }

        // Indeterminate bar: a segment sliding inside its track. It says "working" without
        // claiming a percentage we do not actually know.
        int track_w = RG_MAX(width / 3, 60);
        int track_x = (width - track_w) / 2;
        int track_y = height - line_h * 2 - 6;
        int seg_w = track_w / 3;
        float slide = (sinf((float)now_ms / 380.0f) + 1.0f) / 2.0f;

        rg_gui_draw_panel(track_x, track_y, track_w, 3, 1, rg_gui_scale_color(pal->divider, 200), C_NONE,
                          (int)(200 * scene_in));
        rg_gui_draw_panel(track_x + (int)((track_w - seg_w) * slide), track_y, seg_w, 3, 1, pal->accent, C_NONE,
                          (int)(255 * scene_in));

        rg_gui_draw_text(0, height - line_h - 2, width, status,
                         rg_gui_blend_color(C_RGB(40, 30, 60), pal->text_dim, (int)(scene_in * 255)), C_TRANSPARENT,
                         RG_TEXT_ALIGN_CENTER);

        // Fade to black at the end so the handover to the launcher is not a hard cut
        float fade = phase(now_ms, T_HOLD, T_FADE_DONE);
        if (fade > 0.0f)
            rg_gui_fill_blend(0, 0, width, height, C_BLACK, (int)(fade * 255));

        rg_gui_set_surface(NULL);
        rg_display_submit(surface, 0);

        int64_t elapsed_ms = (rg_system_timer() - start) / 1000 - now_ms;
        rg_task_delay(RG_MAX(FRAME_MS - (int)elapsed_ms, 1));
        rg_system_tick(0);
    }

    rg_display_sync(true);
    // The GUI keeps the last full-screen surface as the backdrop for its overlays, so it has to let
    // go of this one before we free it.
    rg_gui_set_backdrop(NULL);
    rg_surface_free(surface);
    rg_display_clear(C_BLACK);

    // Do not let the keypress that skipped the animation also select something in the launcher
    rg_input_wait_for_key(RG_KEY_ALL, false, 500);
}
