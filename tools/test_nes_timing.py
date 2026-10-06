"""Compile the real NES frame loop/APU with mocked CPU, display and memory.

Run with --cc /path/to/clang (or GCC on Linux, or Zig). Uses a freestanding shared library, including on
Windows without a native C runtime. This does not test physical I2S timing.
"""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
NES = ROOT / "retro-core/components/nofrendo/nes"


def section(path, start, end):
    text = path.read_text()
    return text[text.index(start):text.index(end, text.index(start))]


PRELUDE = r'''
#include <stddef.h>
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef _Bool bool;
#define true 1
#define false 0
#define MESSAGE_ERROR(...) ((void)0)
#define memset test_memset
#define calloc test_calloc
#define free test_free
#define apu_process real_apu_process
#include "apu.h"
#undef apu_process
#include "ppu.h"
static short storage[4096];
static void *test_memset(void *dest, int value, size_t count) {
    unsigned char *p = dest; while (count--) *p++ = value; return dest;
}
static void *test_calloc(size_t count, size_t size) {
    if (count * size > sizeof(storage)) return NULL;
    return test_memset(storage, 0, sizeof(storage));
}
static void test_free(void *p) {}
void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = dest; const unsigned char *s = src;
    while (count--) *d++ = *s++;
    return dest;
}
int test_entry(void *module, unsigned reason, void *reserved) { return 1; }
typedef struct nes_s nes_t;
typedef struct { void (*vblank)(nes_t *); void (*hblank)(nes_t *); } mapper_t;
struct nes_s {
    apu_t *apu; ppu_t *ppu; mapper_t *mapper;
    uint8 *vidbuf;
    int refresh_rate, scanlines_per_frame, scanline, cycles, cycles_per_scanline;
    int cpu_clock, overscan;
    void (*audio_func)(int, int); void (*blit_func)(uint8 *);
};
static nes_t nes;
static nes_t *nes_getptr(void) { return &nes; }
static void nes6502_irq(void) {}
static void nes6502_nmi(void) {}
static void nes6502_burn(int cycles) {}
static int nes6502_execute(int cycles) { return cycles; }
static uint8 mem_getbyte(uint32 addr) { return addr & 255; }
static int rendered, presented, submitted, processed, failure, synth_calls;
static int first_delivery, last_delivery;
void ppu_renderline(uint8 *buffer, int line, bool draw) { rendered += draw; }
void ppu_endline(void) {}
static void apu_process(short *, size_t, bool);
'''

RUNNER = r'''
static int constant_tone(void) { return 1234; }
static const apuext_t tone_ext = {NULL, constant_tone};
static ppu_t test_ppu;
static mapper_t test_mapper;
static uint8 pixels[272 * 240];
static void record_audio(int offset, int count) {
    if (offset != submitted || count <= 0 || offset + count > processed) failure = 1;
    if (submitted == 0) first_delivery = nes.scanline;
    last_delivery = nes.scanline;
    submitted += count;
}
static void record_video(uint8 *buffer) {
    if (!submitted || submitted != processed) failure = 4;
    presented++;
}
static void apu_process(short *buffer, size_t count, bool stereo) {
    int channels = stereo ? 2 : 1;
    if (buffer != apu.buffer + processed * channels) failure = 2;
    if (processed + count > 32000 / nes.refresh_rate + 1) failure = 3;
    real_apu_process(buffer, count, stereo);
    processed += count; synth_calls++;
}
static void setup(int rate, int refresh, bool stereo) {
    test_memset(&nes, 0, sizeof(nes));
    nes.refresh_rate = refresh;
    nes.scanlines_per_frame = refresh == 50 ? 312 : 262;
    nes.cpu_clock = refresh == 50 ? 1662607 : 1789773;
    nes.cycles_per_scanline = nes.cpu_clock / refresh / nes.scanlines_per_frame;
    nes.apu = apu_init(rate, stereo);
    nes.ppu = &test_ppu; nes.mapper = &test_mapper; nes.vidbuf = pixels;
    nes.audio_func = record_audio; nes.blit_func = record_video;
    apu_reset(); apu_setopt(APU_FILTER_TYPE, APU_FILTER_NONE);
    apu_setext(&tone_ext);
    rendered = presented = submitted = processed = failure = synth_calls = 0;
}
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_timing(int rate, int refresh, int stereo, int draw, int initial_line) {
    setup(rate, refresh, stereo);
    nes.scanline = initial_line;
    int total = 0;
    for (int frame = 0; frame < refresh; frame++) {
        submitted = processed = synth_calls = 0;
        int start_line = nes.scanline;
        nes_emulate(draw);
        if (failure || submitted != processed) return 10 + failure;
        // Bound synthesis overhead even when every scanline is emulated.
        if (synth_calls > 5) return 24;
        total += submitted;
        if (nes.scanline != 0) return 20;
        if (start_line == 0) {
            if (first_delivery <= 0 || first_delivery > nes.scanlines_per_frame / 4 + 2
                || (last_delivery != 0 && last_delivery != nes.scanlines_per_frame)) return 21;
            // Samples must stay contiguous, including across delivery batches.
            for (int offset = 0; offset < submitted; offset++) {
                if (apu.buffer[offset * (stereo ? 2 : 1)] != 1234) return 22;
                if (stereo && apu.buffer[offset * 2 + 1] != 1234) return 23;
            }
        }
    }
    if (total != rate) return 30;
    if (presented != (draw ? refresh : 0)) return 31;
    if (rendered != (draw ? refresh * nes.scanlines_per_frame - initial_line : 0)) return 32;
    apu.sample_remainder = 49;
    apu_reset();
    if (apu.sample_remainder != 0) return 33;
    return 0;
}
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_filter(int filter, int stereo) {
    // Real pulse/noise/triangle/DMC mixing must be invariant under batching.
    setup(32000, 60, stereo);
    apu_setext(NULL); apu_setopt(APU_FILTER_TYPE, filter);
    apu_write(APU_SMASK, 0x1f);
    apu_write(APU_WRA0, 0xbf); apu_write(APU_WRA2, 100); apu_write(APU_WRA3, 0xf8);
    apu_write(APU_WRC0, 0xff); apu_write(APU_WRC2, 80); apu_write(APU_WRC3, 0xf8);
    apu_write(APU_WRD0, 0x3f); apu_write(APU_WRD2, 5); apu_write(APU_WRD3, 0xf8);
    apu_write(APU_WRE0, 0x4f); apu_write(APU_WRE1, 64);
    apu_write(APU_WRE2, 0); apu_write(APU_WRE3, 15);
    apu_t original = apu;
    short whole[1280], chunks[1280];
    real_apu_process(whole, 640, stereo);
    apu_t result = apu;
    apu = original;
    for (int offset = 0; offset < 640;) {
        int count = (offset % 7) + 1;
        if (count > 640 - offset) count = 640 - offset;
        real_apu_process(chunks + offset * (stereo ? 2 : 1), count, stereo);
        offset += count;
    }
    for (int i = 0; i < 640 * (stereo ? 2 : 1); i++)
        if (whole[i] != chunks[i]) return 40;
    if (apu.prev_sample != result.prev_sample) return 41;
    return 0;
}
'''

DISPLAY_STUB = r'''
typedef struct { int width, height, stride, offset; } rg_surface_t;
static rg_surface_t surface;
static rg_surface_t *currentUpdate = &surface;
static bool busy;
static int overscan, autocrop, display_calls;
#define NES_SCREEN_WIDTH 256
#define NES_SCREEN_HEIGHT 240
static bool rg_display_sync(bool block) { return !busy; }
static void rg_display_submit(const rg_surface_t *s, unsigned flags) { display_calls++; }
'''

DISPLAY_RUNNER = r'''
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_display(void) {
    busy = true; display_calls = 0;
    blit_screen(pixels);
    // A busy previous frame must not discard the next completed frame.
    if (display_calls != 1) return 50;
    busy = false;
    blit_screen(pixels);
    if (display_calls != 2) return 51;
    busy = true;
    blit_screen(NULL);
    if (display_calls != 3) return 52;
    for (int skipFrames = 0; skipFrames <= 1; skipFrames++)
        for (int nsfPlayer = 0; nsfPlayer <= 1; nsfPlayer++)
            for (busy = false; ; busy = true) {
                bool drawFrame = DRAW_EXPRESSION;
                // The second framebuffer can be drawn while the first is busy.
                if (drawFrame != (!skipFrames && !nsfPlayer)) return 53;
                if (busy) break;
            }
    return 0;
}
'''

POLICY_STUB = r'''
typedef struct { int frameskip, frameskipValue; bool frameskipManual; float speed; } test_app_t;
static test_app_t test_app;
static int policy_elapsed;
static int rg_system_timer(void) { return policy_elapsed; }
static void nsf_draw_overlay(void) {}
POLICY_HELPERS
static int frame_policy(int skipFrames, bool nsfPlayer) {
    test_app_t *app = &test_app;
    int startTime = 0;
    bool drawFrame = !skipFrames && !nsfPlayer;
'''

POLICY_RUNNER = r'''
    return skipFrames;
}
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_frame_policy(void) {
    nes_getptr()->refresh_rate = 60; test_app.speed = 1; test_app.frameskip = 0;
    const int within_budget[] = {1000, 10000, 16666, 18166};
    for (int i = 0; i < 4; i++) {
        policy_elapsed = within_budget[i];
        busy = true;
        if (frame_policy(0, false) != 0) return 70;
        busy = false;
        if (frame_policy(0, false) != 0) return 71;
    }
    policy_elapsed = 18167;
    if (frame_policy(0, false) != 1) return 72;
    if (frame_policy(2, false) != 1) return 73;
    test_app.frameskip = 2; policy_elapsed = 1000;
    if (frame_policy(0, false) != 2) return 74;
    if (frame_policy(0, true) != 10) return 75;
    test_app.frameskipManual = true; policy_elapsed = 50000;
    for (int n = 0; n <= 5; ++n) {
        test_app.frameskipValue = n;
        if (frame_policy(0, false) != n) return 76;
    }
    return 0;
}
'''

PPU_STUB = r'''
#undef OPT
#define OPT(n) ppu.options[n]
#define INLINE static inline __attribute__((__always_inline__))
#define IRAM_ATTR
#define BG_TRANS 0x80
#define SP_PIXEL 0x40
#define BG_CLEAR(v) ((v) & BG_TRANS)
#define BG_SOLID(v) (BG_CLEAR(v) == 0)
#define SP_CLEAR(v) (((v) & SP_PIXEL) == 0)
#define FULLBG (ppu.palette[0] | BG_TRANS)
static ppu_t ppu;
static uint8 chr_memory[0x4000];
static unsigned pattern_reads;
static uint8 pattern_read(unsigned address) { pattern_reads++; return chr_memory[address]; }
#define PPU_MEM_READ(x) pattern_read(x)
static int nes6502_getcycles(void) { return 100; }
'''

PPU_RUNNER = r'''
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_sprites(void) {
    test_memset(&ppu, 0, sizeof(ppu));
    test_memset(ppu.oam, 255, sizeof(ppu.oam));
    ppu.obj_on = ppu.left_obj_on = true; ppu.obj_height = 8;
    ppu_obj_t *sprite = (ppu_obj_t *)ppu.oam;
    *sprite = (ppu_obj_t){9, 1, 0, 0};
    ppu.palette[17] = 10; ppu.palette[18] = 20; ppu.palette[19] = 30;
    for (unsigned bits = 0; bits < 65536; bits++) {
        chr_memory[16] = bits & 255; chr_memory[24] = bits >> 8;
        for (int mode = 0; mode < 4; mode++) {
            sprite->attr = (mode & 1 ? OAMF_HFLIP : 0) | (mode & 2 ? OAMF_BEHIND : 0);
            uint8 line[272];
            for (int i = 0; i < 272; i++) line[i] = (i & 1) ? 7 : BG_TRANS;
            ppu.strikeflag = false; pattern_reads = 0;
            ppu_renderoam(line, 10, true);
            if (pattern_reads != 2) return 60; // sprite-zero strike and drawing share one fetch
            for (int i = 0; i < 8; i++) {
                int bit = mode & 1 ? i : 7 - i;
                int color = ((bits >> bit) & 1) | (((bits >> (8 + bit)) & 1) << 1);
                int original = (i & 1) ? 7 : BG_TRANS;
                int expected = original;
                if (color) expected = SP_PIXEL | ((mode & 2) && (i & 1) ? original : color * 10);
                if (line[i] != expected) return 61;
            }
        }
    }
    // A skipped frame still evaluates sprite zero, without writing video.
    chr_memory[16] = 255; chr_memory[24] = 0;
    ppu.strikeflag = false; pattern_reads = 0;
    ppu_renderoam(NULL, 10, false);
    if (!ppu.strikeflag || pattern_reads != 2) return 62;
    ppu.strikeflag = true; pattern_reads = 0;
    ppu_renderoam(NULL, 10, false);
    if (pattern_reads != 0) return 63;
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang", help="Path to native Clang, GCC (Linux), or Zig")
    args = parser.parse_args()
    apu_source = (NES / "apu.c").read_text().replace('#include "nes.h"', '')
    apu_source = apu_source.replace("apu_process(", "real_apu_process(")
    loop = section(NES / "nes.c", "void nes_emulate(bool draw)", "uint8 *nes_setvidbuf")
    adapter = ROOT / "retro-core/main/main_nes.c"
    blit = section(adapter, "static void blit_screen", "static void submit_audio")
    draw_expression = adapter.read_text().split("bool drawFrame = ", 1)[1].split(";", 1)[0]
    # Execute the adapter's actual end-of-frame policy, excluding the outer loop's closing brace.
    policy = section(adapter, "        if (skipFrames == 0)", "    RG_PANIC(").rstrip().rsplit("}", 1)[0]
    source = (PRELUDE + apu_source + loop + RUNNER + DISPLAY_STUB
              + "\n#define nes nes_getptr()\n" + blit + "\n#undef nes\n"
              + DISPLAY_RUNNER.replace("DRAW_EXPRESSION", draw_expression)
              + "\n#define nes nes_getptr()\n" + POLICY_STUB.replace("POLICY_HELPERS",
                  "#define app test_app\n" + section(ROOT / "components/retro-go/rg_system.c",
                      "int rg_system_get_frameskip", "void rg_system_set_app_speed") + "\n#undef app\n")
              + policy + POLICY_RUNNER
              + "\n#undef nes\n"
              + PPU_STUB + section(NES / "ppu.c", "INLINE uint32 get_patpix", "bool ppu_enabled")
              + PPU_RUNNER)
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
        temp = Path(directory)
        c_file = temp / "nes_test.c"
        c_file.write_text(source)
        library = temp / ("nes_test.dll" if os.name == "nt" else "nes_test.so")
        zig = Path(args.cc).stem == "zig"
        command = [args.cc] + (["cc"] if zig else [])
        command += ["-O2", "-shared", "-ffreestanding", "-fno-builtin", "-nostdlib",
                   "-Werror=implicit-function-declaration", "-I", str(NES)]
        if os.name == "nt" and zig:
            command += ["-target", "x86_64-windows-gnu", "-mno-stack-arg-probe",
                        "-Wl,--entry,test_entry"]
        elif os.name == "nt":
            command += ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld",
                        "-mno-stack-arg-probe",
                        "-Wl,/noentry", "-Wl,/nodefaultlib"]
        else:
            command += ["-fPIC"]
        subprocess.run(command + [str(c_file), "-o", str(library)], check=True)
        dll = ctypes.CDLL(str(library))
        dll.check_timing.argtypes = [ctypes.c_int] * 5
        dll.check_filter.argtypes = [ctypes.c_int] * 2
        scenarios = 0
        for rate in (22050, 32000):
            for refresh in (50, 60):
                for stereo in (0, 1):
                    for draw in (0, 1):
                        for initial in (0, 241):
                            result = dll.check_timing(rate, refresh, stereo, draw, initial)
                            assert result == 0, (rate, refresh, stereo, draw, initial, result)
                            scenarios += 1
        for filter_type in range(3):
            for stereo in (0, 1):
                result = dll.check_filter(filter_type, stereo)
                assert result == 0, (filter_type, stereo, result)
        assert dll.check_display() == 0
        assert dll.check_frame_policy() == 0
        assert dll.check_sprites() == 0
        if os.name == "nt":
            free = ctypes.windll.kernel32.FreeLibrary
            free.argtypes = [ctypes.c_void_p]
            free(dll._handle)
    print(f"PASS: {scenarios} NES timing scenarios, all APU filters, display/redraw/frame-skip policy, "
          "262144 sprite pattern/flip/priority cases")


if __name__ == "__main__":
    main()
