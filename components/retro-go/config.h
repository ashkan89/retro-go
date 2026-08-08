#if defined(RG_TARGET_BRUTZELBOY)
#include "targets/brutzelboy/config.h"
#elif defined(RG_TARGET_ODROID_GO)
#include "targets/odroid-go/config.h"
#elif defined(RG_TARGET_MRGC_G32)
#include "targets/mrgc-g32/config.h"
#elif defined(RG_TARGET_RETRO_ESP32)
#include "targets/retro-esp32/config.h"
#elif defined(RG_TARGET_RETRO_RULER_V1)
#include "targets/retro-ruler-V1/config.h"
#elif defined(RG_TARGET_SDL2)
#include "targets/sdl2/config.h"
#elif defined(RG_TARGET_MRGC_GBM)
#include "targets/mrgc-gbm/config.h"
#elif defined(RG_TARGET_ESPLAY_MICRO)
#include "targets/esplay-micro/config.h"
#elif defined(RG_TARGET_ESP32_S3_DEVKIT)
#include "targets/esp32-s3-devkit/config.h"
#elif defined(RG_TARGET_ESP32_S3_N8R2_ST7796)
#include "targets/esp32-s3-n8r2-st7796/config.h"
#elif defined(RG_TARGET_ESP32_S3_N16R8_ST7796)
#include "targets/esp32-s3-n16r8-st7796/config.h"
#elif defined(RG_TARGET_ESP32_S3_N8R2_ST7789V2)
#include "targets/esp32-s3-n8r2-st7789v2/config.h"
#elif defined(RG_TARGET_ESP32_S3_N16R8_ST7789V2)
#include "targets/esp32-s3-n16r8-st7789v2/config.h"
#elif defined(RG_TARGET_ESP32_S3_N8R2_ILI9341)
#include "targets/esp32-s3-n8r2-ili9341/config.h"
#elif defined(RG_TARGET_ESP32_S3_N16R8_ILI9341)
#include "targets/esp32-s3-n16r8-ili9341/config.h"
#elif defined(RG_TARGET_FRI3D_2024)
#include "targets/fri3d-2024/config.h"
#elif defined(RG_TARGET_BYTEBOI_REV1)
#include "targets/byteboi-rev1/config.h"
#elif defined(RG_TARGET_RACHEL_ESP32)
#include "targets/rachel-esp32/config.h"
#elif defined(RG_TARGET_NULLNANO)
#include "targets/nullnano/config.h"
#elif defined(RG_TARGET_T_DECK_PLUS)
#include "targets/t-deck-plus/config.h"
#elif defined(RG_TARGET_VMU)
#include "targets/vmu/config.h"
#elif defined(RG_TARGET_CROKPOCKET)
#include "targets/crokpocket/config.h"
#elif defined(RG_TARGET_REDROID_GO)
#include "targets/redroid-go/config.h"
#else
#warning "No target defined. Defaulting to ODROID-GO."
#include "targets/odroid-go/config.h"
#define RG_TARGET_ODROID_GO
#endif

#ifndef RG_PROJECT_NAME
#define RG_PROJECT_NAME "Retro-Go"
#endif

#ifndef RG_PROJECT_WEBSITE
#define RG_PROJECT_WEBSITE "https://github.com/ashkan89/retro-go"
#endif

#ifndef RG_PROJECT_CREDITS
#define RG_PROJECT_CREDITS \
    "Retro-Go: ducalex\n"
    // TODO: Decide which additional credits should be included here?
    // Maybe the main author for each emulator? Or should that credit be only when seeing `about` inside said app?
    // What about libraries and fonts (lodepng, etc)?
    // What about targets/ports? Should we have a RG_TARGET_AUTHOR to append here?
#endif

#ifndef RG_PROJECT_APP
#define RG_PROJECT_APP "unknown"
#endif

#ifndef RG_PROJECT_VER
#define RG_PROJECT_VER "unknown"
#endif

#ifndef RG_BUILD_INFO
#define RG_BUILD_INFO "(none)"
#endif

#ifndef RG_BUILD_TIME
// 2020-01-31 00:00:00, first retro-go commit :)
#define RG_BUILD_TIME 1580446800
#endif

#ifndef RG_BUILD_DATE
#define RG_BUILD_DATE __DATE__ " " __TIME__
#endif

#ifndef RG_APP_LAUNCHER
#define RG_APP_LAUNCHER "launcher"
#endif

#ifndef RG_APP_FACTORY
#define RG_APP_FACTORY NULL
#endif

#ifndef RG_UPDATER_ENABLE
#define RG_UPDATER_ENABLE 1
#endif

// If either of the following isn't defined then the updater will only perform version *checks*, not self-update
// #define RG_UPDATER_APPLICATION       RG_APP_FACTORY
// #define RG_UPDATER_DOWNLOAD_LOCATION RG_STORAGE_ROOT "/odroid/firmware"

#ifndef RG_UPDATER_GITHUB_RELEASES
#define RG_UPDATER_GITHUB_RELEASES "https://api.github.com/repos/ashkan89/retro-go/releases?per_page=10"
#endif

#ifndef RG_PATH_MAX
#define RG_PATH_MAX 255
#endif

#ifndef RG_RECOVERY_BTN
#define RG_RECOVERY_BTN RG_KEY_ANY
#endif

// Headphone jack support. The headphone DAC hangs off the same I2S bus as the speaker amplifier
// and a GPIO tells us which one to enable, so this costs no extra peripheral.
#ifndef RG_AUDIO_USE_HEADPHONE_JACK
#define RG_AUDIO_USE_HEADPHONE_JACK 0
#endif

// Level read on RG_GPIO_SND_HP_DETECT while a plug is inserted. Keep this the *opposite* of the
// pin's idle level (pulled up, so 0) or an unwired board would think it always has headphones.
#ifndef RG_GPIO_SND_HP_DETECT_LEVEL
#define RG_GPIO_SND_HP_DETECT_LEVEL 0
#endif

// How long the detect line must hold a new state before we switch outputs
#ifndef RG_AUDIO_HP_DEBOUNCE_MS
#define RG_AUDIO_HP_DEBOUNCE_MS 200
#endif

// How often the detect line is sampled while no audio is being submitted
#ifndef RG_AUDIO_HP_POLL_INTERVAL_MS
#define RG_AUDIO_HP_POLL_INTERVAL_MS 50
#endif

// Volume is remembered per route. Headphones sit in your ears and need far less drive than a
// speaker, so their first-run default is deliberately low.
#ifndef RG_AUDIO_HP_DEFAULT_VOLUME
#define RG_AUDIO_HP_DEFAULT_VOLUME 25
#endif

#ifndef RG_BATTERY_CALC_PERCENT
#define RG_BATTERY_CALC_PERCENT(raw) (100)
#endif

#ifndef RG_BATTERY_CALC_VOLTAGE
#define RG_BATTERY_CALC_VOLTAGE(raw) (0)
#endif

// These values are to prevent jitter, so that the battery icon doesn't flicker or
// percent display doesn't oscillate between 77 and 78%, for example
#ifndef RG_BATTERY_UPDATE_THRESHOLD
#define RG_BATTERY_UPDATE_THRESHOLD 1.0f
#endif
#ifndef RG_BATTERY_UPDATE_THRESHOLD_VOLT
#define RG_BATTERY_UPDATE_THRESHOLD_VOLT 0.010f
#endif

// Number of cycles the hardware state must be maintained before the change is reflected in rg_input_read_gamepad.
// The reaction time is calculated as such: N*10ms +/- 10ms. Different hardware types have different requirements.
// Valid range is 1-9
#ifndef RG_GAMEPAD_DEBOUNCE_PRESS
#define RG_GAMEPAD_DEBOUNCE_PRESS (2)
#endif
#ifndef RG_GAMEPAD_DEBOUNCE_RELEASE
#define RG_GAMEPAD_DEBOUNCE_RELEASE (2)
#endif
// Wait for ADC value to be stable before registering it (values of 50 - 250 are typically good)
#ifndef RG_GAMEPAD_ADC_FILTER_WINDOW
#define RG_GAMEPAD_ADC_FILTER_WINDOW (150)
#endif

#ifndef RG_HAPTIC_INPUT_FEEDBACK_MS
#define RG_HAPTIC_INPUT_FEEDBACK_MS (50)
#endif

#ifndef RG_LOG_COLORS
#define RG_LOG_COLORS (1)
#endif

#ifndef RG_TICK_RATE
#ifdef ESP_PLATFORM
#define RG_TICK_RATE CONFIG_FREERTOS_HZ
#else
#define RG_TICK_RATE 1000
#endif
#endif

// The ESP32-S3 display path is CPU-rendered and transferred with SPI DMA.  Do
// not let an emulator advertise a refresh rate above the useful LCD ceiling;
// excess frames only steal time from audio synthesis and input processing.
#ifndef RG_MAX_FPS
#define RG_MAX_FPS 60
#endif

#ifndef RG_TASK_AFFINITY_MAIN
#define RG_TASK_AFFINITY_MAIN 0
#endif

#ifndef RG_TASK_AFFINITY_SYSTEM
#define RG_TASK_AFFINITY_SYSTEM -1
#endif

#ifndef RG_TASK_AFFINITY_IO
#define RG_TASK_AFFINITY_IO 1
#endif

#ifndef RG_TASK_AFFINITY_AUDIO
#define RG_TASK_AFFINITY_AUDIO RG_TASK_AFFINITY_IO
#endif

#ifndef RG_ZIP_SUPPORT
#define RG_ZIP_SUPPORT 1
#endif

#ifndef RG_SCREEN_PARTIAL_UPDATES
#define RG_SCREEN_PARTIAL_UPDATES 1
#endif

#ifndef RG_SCREEN_SAFE_AREA
#define RG_SCREEN_SAFE_AREA {0, 0, 0, 0}
#endif

#ifndef RG_SCREEN_VISIBLE_AREA
#define RG_SCREEN_VISIBLE_AREA {0, 0, 0, 0}
#endif

#ifndef RG_LANG_DEFAULT
#define RG_LANG_DEFAULT RG_LANG_EN
#endif

#ifndef RG_FONT_DEFAULT
#define RG_FONT_DEFAULT RG_FONT_VERA_11
#endif
