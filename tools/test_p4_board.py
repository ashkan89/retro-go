"""Check WT9932P4-TINY pin ownership and production SDMMC/USB initialization."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ['esp32-p4-n16r32-st7796', 'esp32-p4-n16r32-ili9341']


def function(source, start):
    begin = source.index(start)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    return source[begin:end] + '\n'


STUBS = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define GPIO_NUM_NC -1
#define SDMMC_HOST_SLOT_0 0
#define SDMMC_HOST_SLOT_1 1
#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_HOST_FLAG_1BIT 1
#define SDMMC_HOST_FLAG_4BIT 2
#define SDMMC_HOST_FLAG_8BIT 4
#define SDMMC_HOST_FLAG_DDR 8
#define SDMMC_HOST_FLAG_DEINIT_ARG 32
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define SDMMC_CMD_READ_SINGLE 17
#define SDMMC_CMD_READ_MULTIPLE 18
#define SDMMC_CMD_WRITE_SINGLE 24
#define SDMMC_CMD_WRITE_MULTIPLE 25
#define C_GREEN 1
#define C_RED 2
typedef int esp_err_t;
typedef struct { int opcode; } sdmmc_command_t;
typedef struct {
    int flags, slot, max_freq_khz;
    int (*init)(void);
    int (*deinit_p)(int);
    int (*do_transaction)(int, sdmmc_command_t *);
} sdmmc_host_t;
typedef struct { sdmmc_host_t host; } sdmmc_card_t;
typedef struct { int clk,cmd,d0,d1,d2,d3,d4,d5,d6,d7,cd,wp,width,flags; } sdmmc_slot_config_t;
static int usb_activity_color, usb_activity_sequence, transfers;
static unsigned allocations, host_inits, force_stops, slot_stops;
static bool power_ok=true, fail_alloc, fail_host, fail_slot, fail_card;
static bool host_alive, card_slot_alive, radio_alive;
static sdmmc_slot_config_t seen_slot;
static bool rg_storage_init_sdcard_power(void) { return power_ok; }
static int sdmmc_host_init(void) {
    assert(power_ok); host_inits++;
    if (fail_host) return ESP_FAIL;
    host_alive=true; return ESP_OK;
}
static int sdmmc_host_deinit_slot(int slot) {
    assert(slot==RG_STORAGE_SDMMC_HOST && card_slot_alive);
    slot_stops++; card_slot_alive=false; host_alive=radio_alive; return ESP_OK;
}
static int sdmmc_host_deinit(void) {
    assert(!radio_alive); force_stops++; host_alive=card_slot_alive=false; return ESP_OK;
}
static int sdmmc_host_init_slot(int number,const sdmmc_slot_config_t *slot) {
    assert(number==RG_STORAGE_SDMMC_HOST && host_alive);
    seen_slot=*slot;
    if (fail_slot) return ESP_FAIL;
    card_slot_alive=true; return ESP_OK;
}
static int sdmmc_host_do_transaction(int slot,sdmmc_command_t *cmd) {
    assert(slot==RG_STORAGE_SDMMC_HOST && cmd); transfers++; return ESP_OK;
}
static int sdmmc_card_init(const sdmmc_host_t *host,sdmmc_card_t *card) {
    assert(host_alive && card_slot_alive && host->slot==RG_STORAGE_SDMMC_HOST);
    assert(host->max_freq_khz==RG_STORAGE_SDMMC_SPEED);
    card->host=*host;
    return fail_card?ESP_FAIL:ESP_OK;
}
static void *test_calloc(size_t count,size_t bytes) {
    if (fail_alloc) return NULL;
    void *p=calloc(count,bytes); if (p) allocations++; return p;
}
static void test_free(void *p) { assert(p && allocations); allocations--; free(p); }
#define SDMMC_HOST_DEFAULT() { .flags=1|2|4|8|32, .slot=1, .max_freq_khz=20000, \
    .init=sdmmc_host_init, .deinit_p=sdmmc_host_deinit_slot, .do_transaction=sdmmc_host_do_transaction }
#define SDMMC_SLOT_CONFIG_DEFAULT() { .clk=43,.cmd=44,.d0=39,.d1=40,.d2=41,.d3=42, \
    .d4=45,.d5=46,.d6=47,.d7=48,.cd=-1,.wp=-1,.width=8 }
'''

RUN = r'''
int main(void) {
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    rg_storage_configure_sdmmc(&host,&slot);
    assert(host.flags & SDMMC_HOST_FLAG_DEINIT_ARG);
    assert(host.flags & SDMMC_HOST_FLAG_1BIT);
    assert(!(host.flags & (SDMMC_HOST_FLAG_8BIT|SDMMC_HOST_FLAG_DDR)));
    assert(host.deinit_p==sdmmc_host_deinit_slot && slot.cd==-1 && slot.wp==-1);
#if TEST_LEGACY
    assert(slot.width==1 && !(host.flags & SDMMC_HOST_FLAG_4BIT));
    assert(slot.clk==14 && slot.cmd==15 && slot.d0==2);
    assert(slot.d1==-1 && slot.d2==-1 && slot.d3==-1);
#else
    assert(host.slot==0 && slot.width==4 && host.max_freq_khz==40000);
    assert(host.flags & SDMMC_HOST_FLAG_4BIT);
    assert(slot.clk==43 && slot.cmd==44 && slot.d0==39);
    assert(slot.d1==40 && slot.d2==41 && slot.d3==42);
    assert(slot.d4==-1 && slot.d5==-1 && slot.d6==-1 && slot.d7==-1);
#endif
    sdmmc_card_t *card=NULL;
    int number=-1;
    power_ok=false;
    assert(init_raw_sdcard(&card,&number)==ESP_FAIL && !allocations && !host_inits);
    power_ok=true; fail_alloc=true;
    assert(init_raw_sdcard(&card,&number)==ESP_ERR_NO_MEM && !host_inits);
    fail_alloc=false; fail_host=true;
    assert(init_raw_sdcard(&card,&number)==ESP_FAIL && !allocations && !host_alive);
    fail_host=false; fail_slot=true;
    assert(init_raw_sdcard(&card,&number)==ESP_FAIL && !allocations && !host_alive && force_stops==1);
    fail_slot=false; fail_card=true;
    assert(init_raw_sdcard(&card,&number)==ESP_FAIL && !allocations && !host_alive && slot_stops==1);
    fail_card=false; radio_alive=host_alive=true;
    assert(init_raw_sdcard(&card,&number)==ESP_OK && allocations==1 && number==host.slot);
    assert(seen_slot.width==slot.width && seen_slot.clk==slot.clk && seen_slot.d3==slot.d3);
    const int commands[]={17,18,24,25,52,5};
    for (unsigned n=0;n<sizeof(commands)/sizeof(commands[0]);n++) {
        sdmmc_command_t cmd={commands[n]};
        assert(card->host.do_transaction(number,&cmd)==ESP_OK);
    }
    assert(usb_activity_sequence==4 && usb_activity_color==C_RED && transfers==6);
    cleanup_usb_card(card,number);
    assert(!allocations && !card_slot_alive && radio_alive && host_alive && slot_stops==2);
    puts("PASS: production SDMMC flags/pins, USB power/init/failure cleanup, activity and slot-scoped ownership");
}
'''


def check_target(target):
    folder = ROOT / 'components/retro-go/targets' / target
    config = (folder / 'config.h').read_text()
    sdk = (folder / 'sdkconfig').read_text()
    assert '#define RG_STORAGE_SDSPI_HOST' not in config
    assert '#define RG_GPIO_SDSPI_' not in config
    assert '#define RG_GPIO_LED_WS2812_RGB' not in config
    assert 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y' in sdk
    assert 'CONFIG_ESP_FORCE_400MHZ_ON_REV_LESS_V3=y' in sdk
    assert 'CONFIG_ESP32P4_REV_MIN_100=y' in sdk
    assert 'CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y' in sdk
    assert 'CONFIG_ESP_HOSTED_SDIO_SLOT_1=y' in sdk
    assert 'CONFIG_ESP_HOSTED_SDIO_1_BIT_BUS=y' in sdk
    pins = {}
    for name, pin in re.findall(r'^#define (RG_(?:GPIO|BATTERY_ADC_GPIO)\w*)\s+GPIO_NUM_(\d+)', config, re.M):
        pins[name] = int(pin)
    for button, pin in re.findall(r'\{(RG_KEY_\w+),\s+\.num = GPIO_NUM_(\d+)', config):
        pins[button] = int(pin)
    radio_keys = ['CLK_SLOT_1', 'CMD_SLOT_1', 'D0_SLOT_1', 'D1_1BIT_BUS_SLOT_1']
    for key in radio_keys:
        pins['RADIO_' + key] = int(re.search(r'CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_' + key + r'=(\d+)', sdk)[1])
    pins['RADIO_RESET'] = int(re.search(r'CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=(\d+)', sdk)[1])
    assert len(pins) == len(set(pins.values())), pins
    # Manufacturer header table: exclude FPC/USB/BOOT/UART/strap pins.
    exposed = set(range(2,7)) | set(range(9,24)) | set(range(26,34)) | set(range(39,55))
    assert set(pins.values()) <= exposed
    assert [pins['RADIO_'+key] for key in radio_keys] == [49,50,52,53]
    assert pins['RADIO_RESET'] == 54
    assert pins['RG_GPIO_LED'] == 51
    assert pins['RG_GPIO_LCD_CS'] == 28 and pins['RG_GPIO_LCD_MOSI'] == 29 and pins['RG_GPIO_LCD_CLK'] == 30
    print(f'PASS: {target}: 400 MHz/pre-v3 profile, onboard SD, all {len(pins)} GPIO assignments unique and exposed')
    return folder


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    compiler = [args.cc] + (['cc'] if Path(args.cc).stem.lower() == 'zig' else [])
    usb = (ROOT / 'components/retro-go/rg_usb_msc.c').read_text()
    native_start = usb.index('#else\nstatic esp_err_t init_raw_sdcard')
    native = function(usb[native_start:], 'static esp_err_t init_raw_sdcard(')
    transaction = function(usb, 'static esp_err_t usb_sdcard_transaction(')
    cleanup_start = usb.rindex('    if (card)\n')
    cleanup = usb[cleanup_start:usb.index('    rg_task_delay(100);', cleanup_start)]
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        (work / 'driver').mkdir()
        (work / 'driver/sdmmc_host.h').write_text(STUBS)
        gpio = '\n'.join(f'#define GPIO_NUM_{n} {n}' for n in range(55))
        modes = [(check_target(target), False) for target in TARGETS] + [(None, True)]
        for folder, legacy in modes:
            config = (f'#include "{(folder / "config.h").as_posix()}"\n' if folder else
                      '#define RG_STORAGE_SDMMC_HOST 1\n#define RG_STORAGE_SDMMC_SPEED 20000\n'
                      '#define RG_GPIO_SDSPI_CLK 14\n#define RG_GPIO_SDSPI_CMD 15\n#define RG_GPIO_SDSPI_D0 2\n')
            source = gpio + '\n' + config + '\n#include "rg_storage_sdmmc.h"\n'
            source += '#define calloc test_calloc\n#define free test_free\n' + transaction + native
            source += 'static void cleanup_usb_card(sdmmc_card_t *card,int sd_slot) {\n' + cleanup + '}\n' + RUN
            harness, executable = work / 'board.c', work / 'board.exe'
            harness.write_text(source)
            subprocess.run(compiler + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2', '-UNDEBUG',
                '-DSOC_SDMMC_USE_GPIO_MATRIX=1', f'-DTEST_LEGACY={int(legacy)}',
                '-I', str(work), '-I', str(ROOT / 'components/retro-go'), str(harness), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
