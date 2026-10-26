#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#if CONFIG_IDF_TARGET_ESP32S3
// Reader only: bounded streaming extraction, including CRC verification.
// The bundled tinfl state layout differs from ESP32-S3 ROM miniz. Give the
// decompressor a private symbol so the linker cannot silently bind it to ROM.
#define tinfl_decompress sdcard_tinfl_decompress
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "../../components/retro-go/libs/miniz/miniz.c"
#endif
#endif
