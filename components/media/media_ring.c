#include <rg_system.h>

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#include "media_ring.h"

#undef RG_LOG_TAG
#define RG_LOG_TAG "MEDIA_IO"

struct media_ring_s
{
    uint8_t *data;
    size_t capacity;  // Always a power of two
    size_t mask;
    uint32_t head; // Write cursor, owned by the producer
    uint32_t tail; // Read cursor, owned by the consumer
    bool aborted;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t space; // Given by the consumer after freeing bytes
    SemaphoreHandle_t data_ready; // Given by the producer after writing bytes
#else
    void *space, *data_ready; // Host polling fallback.
#endif
    char name[16];
};

static size_t round_pow2(size_t v)
{
    size_t p = 64;
    while (p < v && p < (1u << 30))
        p <<= 1;
    return p;
}

media_ring_t *media_ring_create(const char *name, size_t capacity, bool psram)
{
    media_ring_t *ring = calloc(1, sizeof(media_ring_t));
    if (!ring)
        return NULL;

    ring->capacity = round_pow2(capacity);
    ring->mask = ring->capacity - 1;
    snprintf(ring->name, sizeof(ring->name), "%s", name ? name : "ring");

    ring->data = rg_alloc(ring->capacity, (psram ? MEM_SLOW : MEM_FAST) | MEM_8BIT | MEM_NOPANIC);
    if (!ring->data)
    {
        // A PSRAM request that cannot be met is not fatal: fall back to whatever is available
        // at a smaller size so playback still works, just with a shorter reserve.
        ring->capacity = round_pow2(capacity / 4 + 1);
        ring->mask = ring->capacity - 1;
        ring->data = rg_alloc(ring->capacity, MEM_ANY | MEM_8BIT | MEM_NOPANIC);
    }

    if (!ring->data)
    {
        RG_LOGE("Failed to allocate ring '%s' (%u bytes)", ring->name, (unsigned)capacity);
        free(ring);
        return NULL;
    }

#ifdef ESP_PLATFORM
    ring->space = xSemaphoreCreateBinary();
    ring->data_ready = xSemaphoreCreateBinary();
    if (!ring->space || !ring->data_ready)
    {
        media_ring_free(ring);
        return NULL;
    }
#endif

    RG_LOGD("Ring '%s' created: %u bytes (%s)", ring->name, (unsigned)ring->capacity,
            psram ? "psram" : "internal");
    return ring;
}

void media_ring_free(media_ring_t *ring)
{
    if (!ring)
        return;
#ifdef ESP_PLATFORM
    if (ring->space)
        vSemaphoreDelete(ring->space);
    if (ring->data_ready)
        vSemaphoreDelete(ring->data_ready);
#endif
    free(ring->data);
    free(ring);
}

size_t media_ring_capacity(const media_ring_t *ring)
{
    return ring ? ring->capacity : 0;
}

size_t media_ring_used(const media_ring_t *ring)
{
    if (!ring)
        return 0;
    // A diagnostic reader can observe different instants. Clamp to the actual capacity.
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    size_t used = (uint32_t)(head - tail);
    return used > ring->capacity ? ring->capacity : used;
}

size_t media_ring_free_space(const media_ring_t *ring)
{
    if (!ring)
        return 0;
    return ring->capacity - media_ring_used(ring);
}

int media_ring_fill_percent(const media_ring_t *ring)
{
    if (!ring || !ring->capacity)
        return 0;
    return (int)((media_ring_used(ring) * 100) / ring->capacity);
}

#ifdef ESP_PLATFORM
static bool wait_sem(SemaphoreHandle_t sem, int timeout_ms)
{
    if (timeout_ms == 0)
        return false;
    TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    // A minimum of one tick, otherwise a sub-tick timeout degenerates into a busy loop
    if (ticks == 0)
        ticks = 1;
    return xSemaphoreTake(sem, ticks) == pdTRUE;
}
#define GIVE_SEM(sem) xSemaphoreGive(sem)
#else
static bool wait_sem(void *sem, int timeout_ms)
{
    (void)sem;
    if (timeout_ms == 0)
        return false;
    rg_task_delay(timeout_ms < 0 ? 2 : (uint32_t)(timeout_ms < 2 ? 2 : timeout_ms));
    return true;
}
#define GIVE_SEM(sem) ((void)0)
#endif

size_t media_ring_write(media_ring_t *ring, const void *data, size_t len, int timeout_ms)
{
    if (!ring || !data || !len)
        return 0;

    const uint8_t *src = data;
    size_t written = 0;
    int64_t deadline = timeout_ms > 0 ? rg_system_timer() + (int64_t)timeout_ms * 1000 : 0;

    while (written < len)
    {
        if (__atomic_load_n(&ring->aborted, __ATOMIC_ACQUIRE))
            break;

        size_t space = media_ring_free_space(ring);
        if (space == 0)
        {
            if (timeout_ms == 0)
                break;
            int remaining = -1;
            if (timeout_ms > 0)
            {
                int64_t left = (deadline - rg_system_timer()) / 1000;
                if (left <= 0)
                    break;
                remaining = (int)left;
            }
            wait_sem(ring->space, remaining);
            continue;
        }

        size_t chunk = len - written;
        if (chunk > space)
            chunk = space;

        size_t offset = ring->head & ring->mask;
        size_t contiguous = ring->capacity - offset;
        if (chunk > contiguous)
            chunk = contiguous;

        memcpy(ring->data + offset, src + written, chunk);
        // Publish the data before the cursor so the consumer never sees uninitialised bytes
        __atomic_store_n(&ring->head, ring->head + chunk, __ATOMIC_RELEASE);
        written += chunk;

        GIVE_SEM(ring->data_ready);
    }

    return written;
}

size_t media_ring_read(media_ring_t *ring, void *data, size_t len, int timeout_ms)
{
    if (!ring || !data || !len)
        return 0;

    uint8_t *dst = data;
    size_t read = 0;
    int64_t deadline = timeout_ms > 0 ? rg_system_timer() + (int64_t)timeout_ms * 1000 : 0;

    while (read < len)
    {
        if (__atomic_load_n(&ring->aborted, __ATOMIC_ACQUIRE))
            break;

        size_t used = media_ring_used(ring);
        if (used == 0)
        {
            if (timeout_ms == 0)
                break;
            int remaining = -1;
            if (timeout_ms > 0)
            {
                int64_t left = (deadline - rg_system_timer()) / 1000;
                if (left <= 0)
                    break;
                remaining = (int)left;
            }
            wait_sem(ring->data_ready, remaining);
            continue;
        }

        size_t chunk = len - read;
        if (chunk > used)
            chunk = used;

        size_t offset = ring->tail & ring->mask;
        size_t contiguous = ring->capacity - offset;
        if (chunk > contiguous)
            chunk = contiguous;

        memcpy(dst + read, ring->data + offset, chunk);
        __atomic_store_n(&ring->tail, ring->tail + chunk, __ATOMIC_RELEASE);
        read += chunk;

        GIVE_SEM(ring->space);
    }

    return read;
}

bool media_ring_wait_readable(media_ring_t *ring, int timeout_ms)
{
    if (!ring || media_ring_aborted(ring))
        return false;
    if (media_ring_used(ring))
        return true;
    wait_sem(ring->data_ready, timeout_ms);
    return !media_ring_aborted(ring) && media_ring_used(ring) != 0;
}

size_t media_ring_peek(const media_ring_t *ring, void *data, size_t len)
{
    if (!ring || !data || !len)
        return 0;

    size_t used = media_ring_used(ring);
    if (len > used)
        len = used;

    size_t offset = ring->tail & ring->mask;
    size_t contiguous = ring->capacity - offset;
    size_t first = len < contiguous ? len : contiguous;

    memcpy(data, ring->data + offset, first);
    if (len > first)
        memcpy((uint8_t *)data + first, ring->data, len - first);

    return len;
}

size_t media_ring_skip(media_ring_t *ring, size_t len)
{
    if (!ring || !len)
        return 0;

    size_t used = media_ring_used(ring);
    if (len > used)
        len = used;

    __atomic_store_n(&ring->tail, ring->tail + len, __ATOMIC_RELEASE);
    GIVE_SEM(ring->space);
    return len;
}

void media_ring_reset(media_ring_t *ring)
{
    if (!ring)
        return;
    __atomic_store_n(&ring->tail, __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    GIVE_SEM(ring->space);
}

void media_ring_abort(media_ring_t *ring)
{
    if (!ring)
        return;
    __atomic_store_n(&ring->aborted, true, __ATOMIC_RELEASE);
    GIVE_SEM(ring->space);
    GIVE_SEM(ring->data_ready);
}

void media_ring_resume(media_ring_t *ring)
{
    if (!ring)
        return;
    __atomic_store_n(&ring->aborted, false, __ATOMIC_RELEASE);
}

bool media_ring_aborted(const media_ring_t *ring)
{
    return ring && __atomic_load_n(&ring->aborted, __ATOMIC_ACQUIRE);
}
