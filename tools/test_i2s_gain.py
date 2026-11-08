"""Check the production I2S gain path, unity bypass, and DMA write failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'components/retro-go/drivers/audio/i2s.c').read_text()
start = source.index('static bool write_dma(')
end = source.index('static bool driver_submit(', start)
code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef struct { int16_t left, right; } rg_audio_frame_t;
typedef int esp_err_t;
#define ESP_OK 0
#define I2S_NUM_0 0
#define RG_AUDIO_I2S_WRITE_TIMEOUT_MS 100
#define pdMS_TO_TICKS(x) (x)
#define RG_LOGW(...) ((void)0)
#define DMA_BUFFER_LEN 192
#define RG_COUNT(x) (sizeof(x)/sizeof((x)[0]))
#define RG_MAX(a,b) ((a)>(b)?(a):(b))
#define RG_AUDIO_USE_INT_DAC 0
#define DEVICE_IS_EXTERNAL(dev) ((dev) >= 1 && (dev) <= 3)
static struct { bool muted; int volume, device, sample_rate, ramp_gain, ramp_target, ramp_step, ramp_remaining; } state;
static rg_audio_frame_t output[640];
static size_t output_count;
static const void *last_pointer;
static int fail_mode;
static esp_err_t i2s_write(int port, const void *data, size_t size, size_t *written, int timeout) {
    assert(timeout == 100);
    last_pointer = data;
    memcpy(output + output_count, data, size);
    output_count += size / sizeof(rg_audio_frame_t);
    *written = fail_mode == 2 ? size / 2 : size;
    return fail_mode == 1 ? -1 : ESP_OK;
}
'''
code += source[start:end]
code += r'''
int main(void) {
    state.sample_rate = 32000;
    rg_audio_frame_t input[192];
    for (int gain = 0; gain <= 100; ++gain) {
        state.volume = gain;
        for (int device = 1; device <= 3; ++device) {
            state.device = device;
            for (int muted = 0; muted <= 1; ++muted) {
                state.muted = muted;
                for (int base = -32768; base <= 32767; base += 192) {
                    size_t n = 0;
                    for (; n < 192 && base + (int)n <= 32767; ++n) {
                        input[n].left = base + (int)n;
                        input[n].right = -1 - input[n].left;
                    }
                    state.ramp_gain = state.ramp_target = (muted ? 0 : gain) * 256;
                    state.ramp_remaining = 0;
                    output_count = 0;
                    assert(write_frames(input, n));
                    assert(output_count == n);
                    if (!muted && gain == 100) assert(last_pointer == input);
                    for (size_t i = 0; i < n; ++i) {
                        assert(output[i].left == input[i].left * (muted ? 0 : gain) / 100);
                        assert(output[i].right == input[i].right * (muted ? 0 : gain) / 100);
                    }
                }
            }
        }
    }
    state.muted = false;
#if RG_AUDIO_VOLUME_RAMP_MS
    // A 5 ms fade must be monotonic, span multiple DMA writes and reach the
    // requested gain exactly; resuming must start from silence.
    for (int i=0;i<192;++i) input[i]=(rg_audio_frame_t){20000,-20000};
    state.volume=100;
    state.ramp_gain=state.ramp_target=25600;
    state.muted=true;
    output_count=0;
    assert(write_frames(input,80));
    assert(output[0].left>19000 && output[79].left>9000 && output[79].left<11000);
    assert(write_frames(input,80));
    assert(output[159].left==0 && output[159].right==0);
    for(int i=1;i<160;i++) assert(output[i].left<=output[i-1].left);
    state.muted=false;
    output_count=0;
    assert(write_frames(input,160));
    assert(output[0].left<1000 && output[159].left==20000);
    for(int i=1;i<160;i++) assert(output[i].left>=output[i-1].left);
#endif
    for (int gain = 50; gain <= 100; gain += 50) {
        state.volume = gain;
        for (fail_mode = 1; fail_mode <= 2; ++fail_mode) {
            output_count = 0;
            assert(!write_frames(input, 192));
        }
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'gain.c'
    exe = Path(tmp) / 'gain'
    src.write_text(code)
    for ramp in (0, 5):
        subprocess.run([args.cc, '-std=c99', '-O2', '-UNDEBUG', f'-DRG_AUDIO_VOLUME_RAMP_MS={ramp}', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('PASS: all int16 samples, gains 0-100, three external routes, mute, unity bypass, short/error writes, 5 ms ramps')
