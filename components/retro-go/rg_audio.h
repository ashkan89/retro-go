#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct // __attribute__((packed))
{
    int16_t left;
    int16_t right;
} rg_audio_frame_t;

typedef rg_audio_frame_t rg_audio_sample_t;

// Where the audio actually comes out. A sink may leave this to the hardware (jack detection),
// in which case the route can change at any time, independently of the selected sink.
typedef enum
{
    RG_AUDIO_ROUTE_UNKNOWN = 0,
    RG_AUDIO_ROUTE_SPEAKER,
    RG_AUDIO_ROUTE_HEADPHONES,
} rg_audio_route_t;

typedef struct
{
    const char *name;                                             // Required
    bool (*init)(int device, int sample_rate);                    // Required
    bool (*deinit)(void);                                         // Required
    bool (*submit)(const rg_audio_frame_t *frames, size_t count); // Required
    bool (*set_mute)(bool mute);                                  // Optional
    bool (*set_volume)(int percent);                              // Optional
    bool (*set_sample_rate)(int sample_rate);                     // Optional
    bool (*set_device)(int device);                               // Optional
    rg_audio_route_t (*get_route)(void);                          // Optional
    const char *(*get_error)(void);                               // Optional
} rg_audio_driver_t;

typedef struct
{
    const rg_audio_driver_t *driver;
    int device;
    const char *name;
    bool automatic; // The driver picks the route itself, so the name alone doesn't describe the output
} rg_audio_sink_t;

typedef struct
{
    int64_t totalSamples;
    int64_t busyTime;
} rg_audio_counters_t;

void rg_audio_init(int sample_rate);
void rg_audio_deinit(void);
void rg_audio_submit(const rg_audio_frame_t *frames, size_t count);
rg_audio_counters_t rg_audio_get_counters(void);

// const char **rg_audio_get_drivers(void);
const char *rg_audio_get_driver(void);

const rg_audio_sink_t *rg_audio_get_sinks(size_t *count);
const rg_audio_sink_t *rg_audio_get_sink(void);
void rg_audio_set_sink(const char *driver_name, int device);
// Name of the active sink, including the resolved route when the sink is automatic ("Auto (Headphones)")
void rg_audio_get_sink_label(char *buffer, size_t length);

rg_audio_route_t rg_audio_get_route(void);
const char *rg_audio_route_name(rg_audio_route_t route);

// Volume is stored and reported per route: headphones need far less drive than a speaker,
// so switching output must not carry a speaker volume over to someone's ears.
int rg_audio_get_volume(void);
void rg_audio_set_volume(int percent);
bool rg_audio_get_mute(void);
void rg_audio_set_mute(bool mute);
int rg_audio_get_sample_rate(void);
void rg_audio_set_sample_rate(int sample_rate);
