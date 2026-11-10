"""Exercise the production I2S producer's wakeups, backpressure and ring wrap."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (ROOT / 'components/retro-go/drivers/audio/i2s.c').read_text()
producer = source[source.index('static bool driver_submit('):source.index('static bool driver_set_mute(')]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define RG_MIN(a,b) ((a)<(b)?(a):(b))
#define portMAX_DELAY 0
typedef void *TaskHandle_t;
typedef struct { int16_t left, right; } rg_audio_frame_t;
static struct {
    TaskHandle_t task;
    bool running;
    rg_audio_frame_t *queue;
    size_t queue_read, queue_write, queue_count;
    void *queue_space;
} state;
static rg_audio_frame_t ring[RG_AUDIO_QUEUE_LENGTH], input[8192];
static size_t output_count, notifications, waits;
static int scheduling, locked;
static bool pending, stop_on_wait;
static void consume(bool all) {
    assert(!locked);
    while (state.queue_count >= DMA_BUFFER_LEN) {
        for (size_t i=0;i<DMA_BUFFER_LEN;i++) {
            assert(output_count < sizeof(input)/sizeof(input[0]));
            rg_audio_frame_t sample=state.queue[state.queue_read];
            assert(sample.left==input[output_count].left && sample.right==input[output_count].right);
            output_count++;
            state.queue_read=(state.queue_read+1)%RG_AUDIO_QUEUE_LENGTH;
            state.queue_count--;
        }
        if (!all) break;
    }
}
static void critical_exit(void) {
    assert(locked); locked=0;
    // The consumer may be running on the other core and drain the block
    // immediately after publication, before the producer sends its wakeup.
    if (scheduling==2) consume(true);
}
#define portENTER_CRITICAL(p) do { assert(!locked); locked=1; } while(0)
#define portEXIT_CRITICAL(p) critical_exit()
static void xTaskNotifyGive(TaskHandle_t task) {
    assert(task==state.task && !locked);
    notifications++; pending=true;
    if (scheduling==0) { consume(true); pending=false; }
}
static void xSemaphoreTake(void *semaphore, int timeout) {
    (void)semaphore; (void)timeout;
    assert(!locked && state.queue_count==RG_AUDIO_QUEUE_LENGTH);
    waits++;
    if (stop_on_wait) { state.running=false; return; }
    consume(false);
}
'''
runner = r'''
static void reset(unsigned position,int mode) {
    state.task=(void*)1; state.running=true; state.queue=ring;
    state.queue_read=state.queue_write=position; state.queue_count=0;
    output_count=notifications=waits=0; pending=stop_on_wait=false; scheduling=mode;
}
int main(void) {
    for (size_t i=0;i<8192;i++) input[i]=(rg_audio_frame_t){(int16_t)i,(int16_t)~i};
    const unsigned bursts[]={1,2,62,133,192,533,2000};
    const unsigned positions[]={0,RG_AUDIO_QUEUE_LENGTH-1,RG_AUDIO_QUEUE_LENGTH/2};
    for (unsigned mode=0;mode<3;mode++)
    for (unsigned b=0;b<sizeof(bursts)/sizeof(bursts[0]);b++)
    for (unsigned p=0;p<sizeof(positions)/sizeof(positions[0]);p++) {
        reset(positions[p],mode);
        for (size_t done=0;done<8192;) {
            size_t n=RG_MIN(bursts[b],8192-done);
            assert(driver_submit(input+done,n)); done+=n;
            if (mode==1) assert(pending || state.queue_count<DMA_BUFFER_LEN);
        }
        consume(true);
        assert(output_count==8192/DMA_BUFFER_LEN*DMA_BUFFER_LEN);
        assert(state.queue_count==8192%DMA_BUFFER_LEN);
        assert(notifications && notifications<=8192/DMA_BUFFER_LEN);
        if (mode==0 && bursts[b]==1) assert(notifications==8192/DMA_BUFFER_LEN);
    }
    reset(0,1);
    assert(driver_submit(input,DMA_BUFFER_LEN-1) && notifications==0);
    assert(driver_submit(input+DMA_BUFFER_LEN-1,1) && notifications==1);
    reset(0,1); stop_on_wait=true;
    assert(!driver_submit(input,RG_AUDIO_QUEUE_LENGTH+1) && waits==1);
    reset(0,0); state.running=false;
    assert(!driver_submit(input,1));
    reset(0,0); state.task=NULL;
    assert(!driver_submit(input,1));
    puts("PASS: producer FIFO, coalesced notifications, full-queue waits, cross-core drain, wrap and stop (63 schedules)");
}
'''
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    harness, executable = folder / 'audio_queue.c', folder / 'audio_queue.exe'
    harness.write_text(stubs + producer + runner)
    for block, capacity in ((8, 19), (192, 1536)):
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-O2', '-UNDEBUG',
                        f'-DDMA_BUFFER_LEN={block}', f'-DRG_AUDIO_QUEUE_LENGTH={capacity}',
                        str(harness), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
