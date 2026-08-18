#pragma once

#include <stdbool.h>

/* Animated boot screen. Returns immediately when it is disabled, when this is not a cold boot,
 * or as soon as the user presses anything. */
void splash_show(bool cold_boot);

bool splash_enabled(void);
void splash_set_enabled(bool enabled);
