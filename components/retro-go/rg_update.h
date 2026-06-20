#pragma once

#include <stdbool.h>

#define RG_UPDATE_IMG_ARG_PREFIX "rg-img:"

bool rg_update_start_image(const char *path);
bool rg_update_handle_pending(const char *args);
