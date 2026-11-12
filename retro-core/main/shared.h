#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rg_system.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE   (32000)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 50 + 1)

// Prefer the conventional FDS BIOS name; retain compatibility with older cards.
static inline const char *nes_fds_bios_path(void)
{
    if (rg_storage_exists(RG_BASE_PATH_BIOS "/disksys.rom"))
        return RG_BASE_PATH_BIOS "/disksys.rom";
    if (rg_storage_exists(RG_BASE_PATH_BIOS "/fds_bios.bin"))
        return RG_BASE_PATH_BIOS "/fds_bios.bin";
    return RG_BASE_PATH_BIOS "/disksys.rom";
}

extern uint8_t shared_memory_block_64K[0x10000];

void launcher_main();
void gbc_main();
void nes_nestopia_main();
void nes_nofrendo_main();
void pce_main();
void sms_main();
void gw_main();
void lynx_main();
void snes_main();

#ifdef __cplusplus
}
#endif
