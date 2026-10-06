"""Exercise the real shared frameskip policy and menu callbacks on a host compiler."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(path, start, end):
    source = path.read_text()
    return source[source.index(start):source.index(end, source.index(start))]


STUBS = r'''
typedef _Bool bool;
#define true 1
#define false 0
#define RG_MIN(a,b) ((a) < (b) ? (a) : (b))
#define RG_MAX(a,b) ((a) > (b) ? (a) : (b))
#define NS_APP 0
#define _(s) s
#define RG_EVENT_SPEEDUP 0
typedef struct {
    int frameskip, frameskipValue, tickRate, frameTime;
    bool frameskipManual;
    float speed;
} rg_app_t;
static rg_app_t app;
static int UPeriod;
static bool saved_mode;
static int saved_value;
static void rg_settings_set_boolean(int ns, const char *key, bool value) { saved_mode = value; }
static void rg_settings_set_number(int ns, const char *key, int value) { saved_value = value; }
static const rg_app_t *rg_system_get_app(void) { return &app; }
static void update_audio_sample_rate(void) {}
static void rg_system_event(int event, void *arg) {}
#define NULL ((void *)0)
typedef enum { RG_DIALOG_INIT, RG_DIALOG_PREV, RG_DIALOG_NEXT, RG_DIALOG_ENTER,
    RG_DIALOG_UPDATE, RG_DIALOG_VOID } rg_gui_event_t;
enum { RG_DIALOG_FLAG_NORMAL, RG_DIALOG_FLAG_HIDDEN };
typedef struct { char *value; int flags; } rg_gui_option_t;
static void strcpy(char *dest, const char *src) { while ((*dest++ = *src++)) {} }
static void sprintf(char *dest, const char *fmt, int value) { dest[0] = '0' + value; dest[1] = 0; }
'''

RUNNER = r'''
int test_entry(void *module, unsigned long reason, void *reserved) { return 1; }
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_frameskip(void) {
    char mode_text[32], value_text[32];
    rg_gui_option_t mode = {mode_text, 0}, value = {value_text, 0};
    app.tickRate = 60; app.speed = 1; app.frameskip = 0;
    frameskip_mode_cb(&mode, RG_DIALOG_INIT);
    frameskip_value_cb(&value, RG_DIALOG_INIT);
    if (mode_text[0] != 'A' || value.flags != RG_DIALOG_FLAG_HIDDEN) return 1;
    if (rg_system_get_next_frameskip(false) != 0 || rg_system_get_next_frameskip(true) != 1) return 2;
    frameskip_mode_cb(&mode, RG_DIALOG_ENTER);
    frameskip_mode_cb(&mode, RG_DIALOG_UPDATE);
    frameskip_value_cb(&value, RG_DIALOG_UPDATE);
    if (mode_text[0] != 'M' || value.flags != RG_DIALOG_FLAG_NORMAL || !saved_mode) return 3;
    for (int n = 0; n <= 5; ++n) {
        if (app.frameskipValue != n || saved_value != n) return 4;
        // Startup defaults and speed changes must not replace a manual choice.
        app.frameskip = 3;
        rg_system_set_app_speed(n % 2 ? 2.0f : 1.0f);
        for (int busy = 0; busy <= 1; ++busy) {
            int skip = 0, rendered = 0;
            for (int frame = 0; frame < 120; ++frame) {
                if (skip) --skip;
                else { ++rendered; skip = rg_system_get_next_frameskip(busy); }
            }
            if (rendered != 120 / (n + 1)) return 5;
        }
        int doom_frames = 0, msx_frames = 0;
        for (int frame = 0; frame < 120; ++frame) {
            doom_frames += I_StartDisplay();
            test_msx_period();
            msx_frames += UPeriod == 100;
        }
        if (doom_frames != 120 / (n + 1) || msx_frames != 120 / (n + 1)) return 11;
        frameskip_value_cb(&value, RG_DIALOG_NEXT);
    }
    if (app.frameskipValue != 0) return 6;
    frameskip_value_cb(&value, RG_DIALOG_PREV);
    if (app.frameskipValue != 5 || value_text[0] != '5') return 7;
    frameskip_mode_cb(&mode, RG_DIALOG_PREV);
    frameskip_value_cb(&value, RG_DIALOG_UPDATE);
    if (saved_mode || value.flags != RG_DIALOG_FLAG_HIDDEN || saved_value != 5) return 8;
    rg_system_set_frameskip(true, -1);
    if (rg_system_get_frameskip() != 0) return 9;
    rg_system_set_frameskip(true, 6);
    if (rg_system_get_frameskip() != 5) return 10;
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    source = STUBS + section(ROOT / "components/retro-go/rg_system.c",
                            "void rg_system_set_frameskip", "float rg_system_get_app_speed")
    source += section(ROOT / "components/retro-go/rg_gui.c",
                      "static rg_gui_event_t frameskip_mode_cb", "static rg_gui_event_t app_options_cb")
    source += section(ROOT / "prboom-go/main/main.c", "bool I_StartDisplay", "void I_EndDisplay")
    source += section(ROOT / "fmsx/main/main.c", "void Keyboard(void)",
                      "    // Keyboard() is a convenient place").replace("void Keyboard(void)",
                                                                          "void test_msx_period(void)") + "}\n"
    source += RUNNER
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
        temp = Path(directory)
        c_file = temp / "frameskip.c"
        c_file.write_text(source)
        library = temp / ("frameskip.dll" if os.name == "nt" else "frameskip.so")
        zig = Path(args.cc).stem == "zig"
        command = [args.cc] + (["cc"] if zig else [])
        command += ["-O2", "-shared", "-ffreestanding", "-fno-builtin", "-nostdlib",
                   "-Werror=implicit-function-declaration"]
        if os.name == "nt" and zig:
            command += ["-target", "x86_64-windows-gnu", "-mno-stack-arg-probe",
                        "-Wl,--entry,test_entry"]
        elif os.name == "nt":
            command += ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld",
                        "-mno-stack-arg-probe", "-Wl,/noentry", "-Wl,/nodefaultlib"]
        else:
            command += ["-fPIC"]
        subprocess.run(command + [str(c_file), "-o", str(library)], check=True)
        dll = ctypes.CDLL(str(library))
        result = dll.check_frameskip()
        assert result == 0, f"Frameskip scenario failed: {result}"
        if os.name == "nt":
            free = ctypes.windll.kernel32.FreeLibrary
            free.argtypes = [ctypes.c_void_p]
            free(dll._handle)
    print("PASS: Auto/Manual menu, 0-5 cadence under load including Doom/fMSX, speed changes, "
          "saved choices, wraparound and bounds")


if __name__ == "__main__":
    main()
