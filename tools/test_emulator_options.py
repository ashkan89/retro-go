"""Run the actual startup option loaders and clock policy with mocked hardware."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile

from test_frameskip import ROOT, STUBS, section


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    stubs = STUBS.replace("int frameskip,", "const char *name; bool isLauncher, initialized; int frameskip,")
    stubs = stubs.replace("saved_value = value;", "if (ns == NS_APP) saved_value = value; else saved_clock = value;")
    stubs = stubs.replace("static int saved_value;", "static int saved_value, saved_clock;")
    stubs += r'''
#define CONFIG_IDF_TARGET_ESP32S3 1
#define NS_GLOBAL 1
#define RG_LOGW(...) ((void)0)
#define RG_LOGI(...) ((void)0)
#define RG_LOGE(...) ((void)0)
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
static int requestedOverclockLevel, overclockLevel, overclockMhz;
static const char *SETTING_OVERCLOCK = "OverclockLevel";
static int commits, writes, divider = 10;
static uint64_t now;
static unsigned cycles;
static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return *a - *b;
}
static int get_default_cpu_mhz(void) { return 240; }
static double rg_settings_get_number(int ns, const char *key, double fallback) {
    return ns == NS_GLOBAL ? saved_clock : saved_value;
}
static bool rg_settings_get_boolean(int ns, const char *key, bool fallback) { return saved_mode; }
static void rg_settings_commit(void) { commits++; }
void rom_i2c_writeReg(uint8_t b, uint8_t h, uint8_t r, uint8_t v) { divider = v; writes++; }
uint8_t rom_i2c_readReg(uint8_t b, uint8_t h, uint8_t r) { return divider; }
uint64_t esp_rtc_get_time_us(void) { return now; }
unsigned xthal_get_ccount(void) { return cycles; }
void ets_update_cpu_frequency(uint32_t speed) {}
static void rg_usleep(int us) { now += us; cycles += us * (240 + (divider - 10) * 10); }
static void rg_task_delay(int ms) { rg_usleep(ms * 1000); }
'''
    system = ROOT / "components/retro-go/rg_system.c"
    source = stubs + section(system, "void rg_system_set_frameskip", "void rg_system_set_app_speed")
    source += section(system, "void rg_system_set_overclock", "char *rg_emu_get_path")
    source += "\nstatic void boot_options(void) {\n"
    source += section(system, "    int savedOverclock =", "    app.configNs =")
    source += section(system, "    app.frameskipManual =", "    if (app.bootFlags & RG_BOOT_RECOVERY)")
    source += r'''
}
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_options(void) {
    app.name = "nes"; app.initialized = true; app.frameskip = 1;
    boot_options();
    rg_system_apply_saved_overclock();
    if (rg_system_get_overclock() || rg_system_get_cpu_speed() != 240 || writes) return 1;
    saved_clock = 6;
    boot_options();
    // A preference must not be displayed as an applied level before startup.
    if (rg_system_get_overclock() != 0) return 2;
    rg_system_apply_saved_overclock();
    if (rg_system_get_overclock() != 6 || rg_system_get_cpu_speed() != 300) return 3;
    int previous_writes = writes;
    rg_system_apply_saved_overclock();
    if (writes != previous_writes) return 4;
    for (int value = 0; value <= 5; value++) {
        rg_system_set_frameskip(true, value);
        app.frameskipManual = false; app.frameskipValue = 0;
        boot_options();
        rg_system_set_overclock(value);
        if (!app.frameskipManual || rg_system_get_next_frameskip(true) != value) return 5;
    }
    rg_system_set_frameskip(false, 5);
    app.frameskip = 3;
    boot_options();
    rg_system_set_overclock(6);
    if (app.frameskipManual || rg_system_get_frameskip() != 3 || app.frameskipValue != 5) return 6;
    int previous_commits = commits;
    int restore = suspend_experimental_overclock();
    if (rg_system_get_overclock() || rg_system_get_cpu_speed() != 240) return 7;
    restore_experimental_overclock(restore);
    if (rg_system_get_overclock() != 6 || app.frameskip != 3 || saved_clock != 6) return 8;
    if (commits != previous_commits) return 9;
    app.name = "launcher"; app.isLauncher = true; overclockLevel = overclockMhz = 0;
    previous_writes = writes;
    rg_system_set_overclock(4);
    rg_system_apply_saved_overclock();
    if (writes != previous_writes || saved_clock != 4 || rg_system_get_cpu_speed() != 240) return 10;
    if (rg_system_get_overclock() != 4) return 11;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
        temp = Path(directory)
        c_file = temp / "options.c"
        c_file.write_text(source)
        library = temp / ("options.dll" if os.name == "nt" else "options.so")
        zig = Path(args.cc).stem == "zig"
        command = [args.cc] + (["cc"] if zig else [])
        command += ["-O2", "-shared", "-ffreestanding", "-fno-builtin", "-nostdlib",
                    "-Werror=implicit-function-declaration"]
        if os.name == "nt" and zig:
            command += ["-target", "x86_64-windows-gnu", "-mno-stack-arg-probe", "-Wl,--entry,test_entry"]
            source = "int test_entry(void *m, unsigned long r, void *p) { return 1; }\n" + source
            c_file.write_text(source)
        elif os.name == "nt":
            command += ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld",
                        "-mno-stack-arg-probe", "-Wl,/noentry", "-Wl,/nodefaultlib"]
        else:
            command += ["-fPIC"]
        subprocess.run(command + [str(c_file), "-o", str(library)], check=True)
        dll = ctypes.CDLL(str(library))
        result = dll.check_options()
        assert result == 0, f"Startup option scenario failed: {result}"
        if os.name == "nt":
            free = ctypes.windll.kernel32.FreeLibrary
            free.argtypes = [ctypes.c_void_p]
            free(dll._handle)
    print("PASS: default clock, saved clock startup, applied-level display, launcher clock, "
          "manual 0-5 reload, Auto reload, clock changes and temporary suspension")


if __name__ == "__main__":
    main()
