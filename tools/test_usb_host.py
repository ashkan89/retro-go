"""Exercise production USB host startup ordering and failures without hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_INTR_FLAG_LEVEL1 1
#define BIT0 1
#define pdMS_TO_TICKS(x) (x)
#define RG_LOGI(...) ((void)0)
#define RG_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef struct {
    bool skip_phy_setup, root_port_unpowered;
    int intr_flags;
    unsigned peripheral_map;
} usb_host_config_t;
static volatile bool task_running, init_done, init_ok;
static int install_error, power_error, deletes;
static char calls[32];
static void record(char c) { size_t n = strlen(calls); calls[n] = c; calls[n+1] = 0; }
static int usb_host_install(const usb_host_config_t *config) {
    record('I');
    assert(!config->skip_phy_setup);
#if TEST_FORCE_FULL_SPEED
    assert(config->root_port_unpowered && config->peripheral_map == BIT0);
#else
    assert(!config->root_port_unpowered && config->peripheral_map == 0);
#endif
    return install_error;
}
#if TEST_FORCE_FULL_SPEED
typedef struct {
    struct { unsigned vbvalidoven, vbvalidovval, avalidoven, avalidovval; } gotgctl_reg;
    bool hnp;
} mock_hw_t;
static mock_hw_t hardware[2];
#define USB_DWC_LL_GET_HW(n) (&hardware[n])
typedef struct { mock_hw_t *dev; } usb_dwc_hal_context_t;
static bool full_speed;
void __real_usb_dwc_hal_init(usb_dwc_hal_context_t *hal, int port_id) {
    hal->dev = USB_DWC_LL_GET_HW(port_id);
    memset(hal->dev, 0, sizeof(*hal->dev)); full_speed = false; record('N');
}
void __real_usb_dwc_hal_core_soft_reset(usb_dwc_hal_context_t *hal) {
    memset(hal->dev, 0, sizeof(*hal->dev)); full_speed = false; record('R');
}
static void usb_dwc_ll_gusbcfg_en_hnp_cap(mock_hw_t *hw) { hw->hnp = true; }
static void usb_dwc_ll_hcfg_set_fsls_supp_only(mock_hw_t *hw) {
    assert(hw == USB_DWC_LL_GET_HW(0)); full_speed = true; record('F');
}
static int usb_host_lib_set_root_port_power(bool enable) {
    assert(enable); assert(strcmp(calls, "IF") == 0); record('P'); return power_error;
}
#endif
static int usb_host_uninstall(void) { record('U'); return ESP_OK; }
static void usb_host_lib_handle_events(int ticks, uint32_t *flags) {
    (void)ticks; (void)flags;
    assert(init_done && init_ok); record('E'); task_running = false;
}
static void vTaskDelete(void *task) { assert(task == NULL); ++deletes; }
'''
RUN = r'''
static void run(int install_result, int power_result, const char *expected, bool ok) {
    install_error = install_result; power_error = power_result;
    calls[0] = 0; deletes = 0; init_done = init_ok = false; task_running = true;
    usb_host_task(NULL);
    assert(init_done && init_ok == ok && !task_running && deletes == 1);
    assert(strcmp(calls, expected) == 0);
}
int main(void) {
#if TEST_FORCE_FULL_SPEED
    run(0, 0, "IFPEU", true);
    run(0, 1, "IFPU", false);
    // A controller reset clears HCFG. Repeated hub reconnects must restore it.
    usb_dwc_hal_context_t hal = {.dev = USB_DWC_LL_GET_HW(0)};
    calls[0] = 0;
    __wrap_usb_dwc_hal_init(&hal, 0);
    assert(full_speed && strcmp(calls, "NF") == 0);
    assert(hal.dev->hnp && hal.dev->gotgctl_reg.vbvalidoven && hal.dev->gotgctl_reg.vbvalidovval);
    assert(hal.dev->gotgctl_reg.avalidoven && hal.dev->gotgctl_reg.avalidovval);
    for (int reconnect = 0; reconnect < 3; ++reconnect) {
        calls[0] = 0;
        __wrap_usb_dwc_hal_core_soft_reset(&hal);
        assert(full_speed && strcmp(calls, "RF") == 0);
        assert(hal.dev->hnp && hal.dev->gotgctl_reg.vbvalidoven && hal.dev->gotgctl_reg.vbvalidovval);
        assert(hal.dev->gotgctl_reg.avalidoven && hal.dev->gotgctl_reg.avalidovval);
    }
    calls[0] = 0;
    __wrap_usb_dwc_hal_init(&hal, 1);
    assert(!full_speed && !hal.dev->hnp && strcmp(calls, "N") == 0);
    assert(!hal.dev->gotgctl_reg.vbvalidoven && !hal.dev->gotgctl_reg.avalidoven);
    calls[0] = 0;
    __wrap_usb_dwc_hal_core_soft_reset(&hal);
    assert(!full_speed && strcmp(calls, "R") == 0);
#else
    run(0, 0, "IEU", true);
#endif
    run(1, 0, "I", false);
    puts("PASS: USB host startup, UTMI host detection, recovery and failure cleanup");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    compiler = [args.cc] + (['cc'] if Path(args.cc).stem.lower() == 'zig' else [])
    source = (ROOT / 'components/retro-go/rg_usb_host.c').read_text()
    task = source[source.index('static void usb_host_task('):source.index('bool rg_usb_host_acquire(')]
    reset = source[source.index('static void configure_p4_host('):
                   source.index('\n#endif\n\n#if defined(ESP_PLATFORM) && (')]
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        harness, executable = work / 'host.c', work / 'host.exe'
        for target in ('esp32-p4-n16r32-st7796', 'esp32-p4-n16r32-ili9341', None):
            harness.write_text(STUBS + (reset if target else '') + task + RUN)
            defines = []
            if target:
                config = ROOT / 'components/retro-go/targets' / target / 'config.h'
                defines = ['-DCONFIG_IDF_TARGET_ESP32P4=1', '-DTEST_FORCE_FULL_SPEED=1',
                           '-DCONFIG_ESP32P4_SELECTS_REV_LESS_V3=1',
                           '-include', str(config)]
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-UNDEBUG'] +
                           defines + [str(harness), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
            print(f'PASS: {target or "non-P4 default host"}', flush=True)


if __name__ == '__main__':
    main()
