"""Exercise the actual I2S consumer loop with a mocked producer and DMA sink.

Run with --cc /path/to/clang. No ESP-IDF hardware or native C runtime is needed.
This checks batching and FIFO ordering, not physical DMA timing.
"""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default="clang")
parser.add_argument("--emit-c", type=Path, help="Generate an ESP-IDF selftest translation unit instead")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / "components/retro-go/drivers/audio/i2s.c").read_text()
start = source.index("static void audio_task(void *arg)")
end = source.index("static bool start_audio_task(void)", start)
consumer = source[start:end]
snes_source = (root / "retro-core/main/main_snes.c").read_text()
snes_start = snes_source.index("void S9xAudioTick(int v_counter, int v_counter_max)")
snes_tick = snes_source[snes_start:snes_source.index("#endif", snes_start)]
msx_source = (root / "fmsx/main/main.c").read_text()
msx_start = msx_source.index("void PlayAllSound(int uSec)")
msx_audio = msx_source[msx_start:msx_source.index("unsigned int WriteAudio", msx_start)]

stub = r'''
#include <stddef.h>
#define memcpy audio_dma_test_memcpy
typedef _Bool bool;
#define true 1
#define false 0
#define RG_AUDIO_USE_HEADPHONE_JACK 0
#define RG_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define RG_MIN(a,b) ((a) < (b) ? (a) : (b))
#define pdTRUE 1
#define portMAX_DELAY 0
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
typedef struct { short left, right; } rg_audio_frame_t;
static rg_audio_frame_t queue[RG_AUDIO_QUEUE_LENGTH];
static struct {
    rg_audio_frame_t *queue;
    size_t queue_read, queue_write, queue_count;
    void *queue_space, *task_stopped;
    bool running;
    unsigned write_errors;
} state;
static unsigned input_total, input_next, burst, output_count, error;
static void *memcpy(void *dest, const void *src, size_t length) {
    unsigned char *d = dest; const unsigned char *s = src;
    while (length--) *d++ = *s++;
    return dest;
}
static unsigned ulTaskNotifyTake(int clear, unsigned timeout) {
    if (input_next == input_total) { state.running = false; return 1; }
    unsigned count = RG_MIN(burst, input_total - input_next);
    count = RG_MIN(count, RG_AUDIO_QUEUE_LENGTH - state.queue_count);
    for (unsigned i = 0; i < count; i++) {
        state.queue[state.queue_write] = (rg_audio_frame_t){
            (short)((int)input_next - 1000), (short)(1000 - (int)input_next)};
        state.queue_write = (state.queue_write + 1) % RG_AUDIO_QUEUE_LENGTH;
        state.queue_count++; input_next++;
    }
    return 1;
}
static int xSemaphoreGive(void *semaphore) { return 1; }
static void vTaskDelete(void *task) {}
static bool write_frames(const rg_audio_frame_t *frames, size_t count) {
    if (count != DMA_BUFFER_LEN) error = 1;
    for (size_t i = 0; i < count; i++, output_count++)
        if (frames[i].left != (short)((int)output_count - 1000)
            || frames[i].right != (short)(1000 - (int)output_count)) error = 2;
    return true;
}
'''
entry = r'''
#ifdef _WIN32
__declspec(dllexport)
#endif
int check_consumer(unsigned chunk, unsigned total, unsigned initial_position) {
    state.queue = queue;
    state.queue_read = state.queue_write = initial_position;
    state.queue_count = state.write_errors = 0;
    state.running = true;
    burst = chunk; input_total = total;
    input_next = output_count = error = 0;
    audio_task(0);
    if (output_count != (total / DMA_BUFFER_LEN) * DMA_BUFFER_LEN) return 3;
    return error;
}
'''
if args.emit_c:
    snes_stubs = r'''
#include <stdint.h>
#define AUDIO_SAMPLE_RATE 32000
#define AUDIO_BUFFER_LENGTH 641
#define AUDIO_LOW_PASS_RANGE 0
static struct { int ROMFramesPerSecond; } Memory;
static bool apu_enabled = true, lowpass_filter;
static rg_audio_frame_t snes_buffer[AUDIO_BUFFER_LENGTH];
static unsigned snes_frames, snes_mixed, snes_blocks, snes_max, snes_error;
#define audioBuffer snes_buffer
#define rg_audio_submit snes_submit
static void snes_submit(const rg_audio_frame_t *frames, size_t count) {
    if (frames < snes_buffer || frames + count > snes_buffer + AUDIO_BUFFER_LENGTH)
        snes_error = 1;
    snes_frames += count; snes_blocks++;
    if (count > snes_max) snes_max = count;
}
static void S9xMixSamples(void *dest, int count) {
    snes_mixed += count / 2;
    if ((rg_audio_frame_t *)dest < snes_buffer || count < 0 || (count & 1)
        || (rg_audio_frame_t *)dest + count / 2 > snes_buffer + AUDIO_BUFFER_LENGTH)
        snes_error = 2;
}
static void S9xMixSamplesLowPass(void *dest, int count, int filter) {
    S9xMixSamples(dest, count);
}
'''
    snes_runner = r'''
#undef audioBuffer
#undef rg_audio_submit
int snes_audio_selftest(void) {
    for (unsigned filter = 0; filter < 2; filter++)
        for (unsigned pal = 0; pal < 2; pal++) {
            Memory.ROMFramesPerSecond = pal ? 50 : 60;
            unsigned lines = pal ? 312 : 262;
            lowpass_filter = filter;
            snes_frames = snes_mixed = snes_blocks = snes_max = snes_error = 0;
            for (int frame = 0; frame < Memory.ROMFramesPerSecond; frame++) {
                for (unsigned line = 1; line < lines; line++) S9xAudioTick(line, lines);
                S9xAudioTick(0, lines);
            }
            if (snes_error || snes_frames != 32000 || snes_mixed != 32000
                || snes_blocks < (unsigned)Memory.ROMFramesPerSecond * 4 || snes_max > 160)
                return 1;
        }
    return 0;
}
'''
    msx_stubs = r'''
typedef void rg_task_t;
typedef struct { unsigned dataInt; } rg_task_msg_t;
static rg_task_t *audioQueue;
static uint64_t audio_remainder, msx_samples;
static int FrameStartTime, msx_error;
static int64_t rg_system_timer(void) { return 0; }
static bool rg_task_send(rg_task_t *task, const rg_task_msg_t *msg) {
    msx_samples += msg->dataInt;
    if (msg->dataInt & 1) msx_error = 1;
    return true;
}
'''
    msx_runner = r'''
int msx_audio_selftest(void) {
    const unsigned intervals[] = {1, 509, 512, 20000, 50000};
    for (unsigned i = 0; i < RG_COUNT(intervals); i++) {
        audio_remainder = msx_samples = msx_error = 0;
        PlayAllSound(0); PlayAllSound(-1);
        for (unsigned n = 0; n < 1000; n++) PlayAllSound(intervals[i]);
        uint64_t frames = (uint64_t)intervals[i] * 1000 * AUDIO_SAMPLE_RATE / 1000000;
        if (msx_error || msx_samples != frames * 2) return 1;
    }
    return 0;
}
'''
    runner = r'''
int audio_dma_selftest(void) {
    const unsigned chunks[] = {1, 2, 62, 133, 533};
    const unsigned totals[] = {DMA_BUFFER_LEN - 1, DMA_BUFFER_LEN,
        DMA_BUFFER_LEN + 1, 2000, 2011};
    const unsigned positions[] = {0, RG_AUDIO_QUEUE_LENGTH - 1,
        RG_AUDIO_QUEUE_LENGTH - DMA_BUFFER_LEN / 2};
    for (unsigned c = 0; c < RG_COUNT(chunks); c++)
        for (unsigned t = 0; t < RG_COUNT(totals); t++)
            for (unsigned p = 0; p < RG_COUNT(positions); p++) {
                int result = check_consumer(chunks[c], totals[t], positions[p]);
                if (result) return result;
            }
    return 0;
}
'''
    args.emit_c.write_text("#define DMA_BUFFER_LEN 192\n#define RG_AUDIO_QUEUE_LENGTH 1536\n"
                          + stub + consumer + entry + runner + snes_stubs + snes_tick + snes_runner
                          + msx_stubs + msx_audio + msx_runner)
    raise SystemExit(0)
with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
    temp = Path(directory)
    c_file = temp / "audio_test.c"
    c_file.write_text(stub + consumer + entry)
    for block, capacity in [(8, 19), (192, 1536)]:
        library = temp / (f"audio_{block}.dll" if os.name == "nt" else f"audio_{block}.so")
        command = [args.cc, "-shared", "-ffreestanding", "-fno-builtin", "-nostdlib"]
        if os.name == "nt":
            command += ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld", "-Wl,/noentry", "-Wl,/nodefaultlib"]
        else:
            command += ["-fPIC"]
        command += [f"-DDMA_BUFFER_LEN={block}", f"-DRG_AUDIO_QUEUE_LENGTH={capacity}",
                    str(c_file), "-o", str(library)]
        subprocess.run(command, check=True)
        dll = ctypes.CDLL(str(library))
        dll.check_consumer.argtypes = [ctypes.c_uint] * 3
        dll.check_consumer.restype = ctypes.c_int
        for chunk in [1, 2, 62, 133, 533]:
            for total in [block - 1, block, block + 1, 2000, 2011]:
                for position in [0, capacity - 1, capacity - block // 2]:
                    result = dll.check_consumer(chunk, total, position)
                    assert result == 0, (block, capacity, chunk, total, position, result)
        if os.name == "nt":
            free = ctypes.windll.kernel32.FreeLibrary
            free.argtypes = [ctypes.c_void_p]
            free.restype = ctypes.c_int
            free(dll._handle)
print("PASS: complete DMA blocks, FIFO order and ring wrap (150 producer scenarios)")
