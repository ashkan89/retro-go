#pragma once
#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void (*init)(const char *path);
    void (*step)(uint32_t keys, rg_surface_t *surface);
    bool (*save)(const char *path);
    bool (*load)(const char *path);
    bool (*reset)(bool hard);
    void (*options)(rg_gui_option_t *dest);
    int width, height, sample_rate, refresh_rate;
} legacy_core_t;
extern legacy_core_t spectrum_core, stella_core, prosystem_core;
extern bool RenderFlag;
void *legacy_read_file(const char *path, size_t *size, size_t limit);
#ifdef __cplusplus
}
#endif
