#include <rg_system.h>

#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#include "media_config.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA"

// Tuned against the two reference boards. LOW must stay comfortably inside the ~2 MB of an
// N8R2 while still leaving room for the launcher's own allocations and the SD driver.
// media_ring_create() rounds its capacity up to a power of two, so these are already
// powers of two: a "384 KB" request would silently become 512 KB of PSRAM.
static const media_profile_t profiles[MEDIA_MEMORY_COUNT] = {
    [MEDIA_MEMORY_LOW] = {
        .profile = MEDIA_MEMORY_LOW,
        .name = "Low",
        .source_buffer = 64 * 1024,
        .network_buffer = 256 * 1024,       // ~16 s of a 128 kbps stream
        .network_buffer_max = 512 * 1024,   // An N8R2 cannot spare more than this for one ring
        .pcm_buffer_frames = 8 * 1024,      // 32 KB, ~186 ms at 44.1 kHz
        .prebuffer_frames = 2 * 1024,
        .prebuffer_ms = 2500,
        .artwork_cache_bytes = 192 * 1024,
        .artwork_cache_entries = 4,
        .artwork_max_dim = 160,
        .thumbnail_dim = 48,
        .fft_size = 128,
        .fft_bands = 16,
        .target_fps = 30,
        .background_blur = false,
        .crossfade_allowed = false,
        .waveform_overview = false,
        .particles = false,
        .library_cache_tracks = 256,
    },
    [MEDIA_MEMORY_NORMAL] = {
        .profile = MEDIA_MEMORY_NORMAL,
        .name = "Normal",
        .source_buffer = 128 * 1024,
        .network_buffer = 512 * 1024,       // ~32 s
        .network_buffer_max = 1024 * 1024,
        .pcm_buffer_frames = 16 * 1024,     // 64 KB, ~372 ms
        .prebuffer_frames = 4 * 1024,
        .prebuffer_ms = 3000,
        .artwork_cache_bytes = 512 * 1024,
        .artwork_cache_entries = 8,
        .artwork_max_dim = 200,
        .thumbnail_dim = 56,
        .fft_size = 256,
        .fft_bands = 20,
        .target_fps = 30,
        .background_blur = true,
        .crossfade_allowed = true,
        .waveform_overview = true,
        .particles = false,
        .library_cache_tracks = 1024,
    },
    [MEDIA_MEMORY_HIGH] = {
        .profile = MEDIA_MEMORY_HIGH,
        .name = "High",
        .source_buffer = 256 * 1024,
        // Icecast servers front-load: live.powerhitz.com pushes ~560 KB in the first two
        // seconds before settling to real time. A ring smaller than that burst forces the
        // reader to stall, and a stalled client is exactly what Icecast drops off its queue
        // and disconnects. Taking the whole burst turns it into ~64 s of reserve instead.
        .network_buffer = 1024 * 1024,
        // A 20 s pre-roll planned for 192 kbps needs ~544 KB, so the default already covers the
        // longest delay offered. The ceiling is only here for a very high bitrate stream.
        .network_buffer_max = 2048 * 1024,
        .pcm_buffer_frames = 32 * 1024,     // 128 KB, ~743 ms
        .prebuffer_frames = 8 * 1024,
        .prebuffer_ms = 4000,
        .artwork_cache_bytes = 1536 * 1024,
        .artwork_cache_entries = 16,
        .artwork_max_dim = 260,
        .thumbnail_dim = 64,
        .fft_size = 512,
        .fft_bands = 24,
        .target_fps = 60,
        .background_blur = true,
        .crossfade_allowed = true,
        .waveform_overview = true,
        .particles = true,
        .library_cache_tracks = 3072,
    },
};

static const media_profile_t *active;
static int forced = -1;

static media_memory_profile_t detect_profile(void)
{
    size_t psram = 0;

#ifdef ESP_PLATFORM
    psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
#else
    psram = 8 * 1024 * 1024; // Host builds are not memory constrained
#endif

    if (psram >= 6 * 1024 * 1024)
        return MEDIA_MEMORY_HIGH;
    if (psram >= 3 * 1024 * 1024)
        return MEDIA_MEMORY_NORMAL;
    return MEDIA_MEMORY_LOW;
}

const media_profile_t *media_profile(void)
{
    if (!active)
    {
        media_memory_profile_t chosen = forced >= 0 ? (media_memory_profile_t)forced : detect_profile();
        if (chosen >= MEDIA_MEMORY_COUNT)
            chosen = MEDIA_MEMORY_LOW;
        active = &profiles[chosen];
        RG_LOGI("Media memory profile: %s (src=%uKB net=%uKB pcm=%u frames fft=%d)", active->name,
                (unsigned)(active->source_buffer / 1024), (unsigned)(active->network_buffer / 1024),
                (unsigned)active->pcm_buffer_frames, active->fft_size);
    }
    return active;
}

void media_profile_override(media_memory_profile_t profile)
{
    if (profile >= MEDIA_MEMORY_COUNT)
        return;
    forced = (int)profile;
    active = NULL;
    media_profile();
}
