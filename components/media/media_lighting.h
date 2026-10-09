#pragma once
#include "media_settings.h"

bool media_lighting_available(void);
bool media_lighting_start(void);
bool media_lighting_stop(void);
/* Bounded integer envelope tap. Never touches the LED driver or waits on a mutex. */
void media_lighting_feed(const int16_t *pcm, size_t frames);
const char *media_lighting_name(media_light_t effect);
/* Pure effect renderer, also exercised by native tests. Components are 0..255. */
uint16_t media_lighting_color(media_light_t effect, int brightness, int level, int bass,
                              bool beat, unsigned hue);
