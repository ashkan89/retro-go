"""Check production MSX frame accounting at long uptimes and with audio waits."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (ROOT / 'fmsx/main/main.c').read_text()
timestamp = re.search(r'^static .* FrameStartTime;', source, re.M).group(0)
keyboard = source[source.index('void Keyboard(void)'):source.index('unsigned int Mouse(')]
audio = source[source.index('void PlayAllSound('):source.index('unsigned int WriteAudio(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define AUDIO_SAMPLE_RATE 32000
static int UPeriod;
static char *PendingLoadSTA;
static uint64_t audio_remainder;
static int64_t now, busy, wait_time;
static void *audioQueue;
typedef struct { unsigned dataInt; } rg_task_msg_t;
static int64_t rg_system_timer(void) { return now; }
static int rg_system_get_frameskip(void) { return 0; }
static void rg_system_tick(int duration) { busy=duration; }
static void LoadSTA(char *path) { (void)path; }
static void rg_task_send(void *task,const rg_task_msg_t *msg) {
    (void)task; assert(msg->dataInt>0 && !(msg->dataInt&1)); now+=wait_time;
}
'''
code += timestamp + '\n' + keyboard + audio + r'''
int main(void) {
    const int64_t starts[]={0,INT32_MAX-10000LL,INT32_MAX+10000LL,UINT32_MAX-10000LL,86400000000LL};
    for (unsigned start=0;start<sizeof(starts)/sizeof(starts[0]);start++)
    for (unsigned blocked=0;blocked<2;blocked++) {
        now=FrameStartTime=starts[start]; audio_remainder=0;
        wait_time=blocked?5000:0;
        for (int frame=0;frame<1000;frame++) {
            now+=11000; PlayAllSound(509); now+=5000; Keyboard();
            assert(busy==16000 && FrameStartTime==now && UPeriod==100);
        }
    }
    puts("PASS: MSX frame accounting across 32-bit boundaries and 24-hour uptime, with/without audio waits (10000 frames)");
}
'''
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    harness, executable = folder / 'msx_timing.c', folder / 'msx_timing.exe'
    harness.write_text(code)
    subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-O2', '-UNDEBUG',
                    str(harness), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
