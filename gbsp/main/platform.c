#include "gpsp_rv32_platform.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>

#if CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT && CONFIG_SPIRAM_XIP_FROM_PSRAM && CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION
#error "gpSP JIT requires executable PSRAM: disable SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION for this app"
#endif

static void *exec_alloc(size_t alignment, size_t size)
{
    if (alignment < 256)
        alignment = 256;
    return heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void exec_free(void *code)
{
    heap_caps_free(code);
}

static bool exec_publish(void *code, size_t size)
{
    if (!code || size > UINTPTR_MAX - (uintptr_t)code)
        return false;
    if (!size)
        return true;
    // IDF 5.5: publish dirty PSRAM data, then invalidate instruction cache.
    uintptr_t start = (uintptr_t)code & ~(uintptr_t)63;
    uintptr_t end = ((uintptr_t)code + size + 63) & ~(uintptr_t)63;
    if (esp_cache_msync((void *)start, end - start,
                       ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK)
        return false;
    if (esp_cache_msync((void *)start, end - start,
                       ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST) != ESP_OK)
        return false;
    __asm__ volatile("fence.i" ::: "memory");
    return true;
}

static void vlog(gpsp_rv32_log_level level, const char *tag, const char *format, va_list args)
{
    char message[256];
    vsnprintf(message, sizeof(message), format, args);
    esp_log_write(level == GPSP_RV32_LOG_WARN ? ESP_LOG_WARN : ESP_LOG_INFO, tag, "%s\n", message);
}

const struct gpsp_rv32_platform_ops gpsp_platform = {
    .exec_alloc = exec_alloc, .exec_free = exec_free, .exec_publish = exec_publish, .vlog = vlog,
};
