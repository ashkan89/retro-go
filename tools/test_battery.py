#!/usr/bin/env python3
"""Check S3 target battery wiring and the actual ADC reader/manual-audio routing."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def compile_run(cc, folder, name, source):
    c = folder / (name + '.c')
    exe = folder / name
    c.write_text(source)
    subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                    str(c), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    targets = ROOT / 'components/retro-go/targets'
    defines = '\n'.join(f'#define GPIO_NUM_{i} {i}' for i in range(49))
    defines += '\n#define GPIO_NUM_NC -1\n#define ADC_UNIT_1 1\n#define ADC_UNIT_2 2\n'
    defines += '\n'.join(f'#define ADC_CHANNEL_{i} {i}\n#define ADC1_CHANNEL_{i} {i}' for i in range(10))
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        count = 0
        for env in sorted(targets.glob('*/env.py')):
            if not re.search(r'["\']esp32s3["\']', env.read_text()):
                continue
            name = env.parent.name
            config = env.with_name('config.h')
            source = '#include <assert.h>\n' + defines + f'\n#include "{config}"\n'
            source += 'int main(void) { assert(RG_BATTERY_DRIVER == 1);'
            if name.startswith('esp32-s3-'):
                pin = 4 if name == 'esp32-s3-devkit' else 1
                source += f'assert(RG_BATTERY_ADC_GPIO=={pin} && RG_BATTERY_ADC_UNIT==1 && RG_BATTERY_ADC_CHANNEL=={pin-1});'
                # The voltage pin must not also be a digital input/output.
                text = config.read_text()
                used = re.findall(r'^#define RG_GPIO_\w+\s+GPIO_NUM_(\d+)', text, re.M)
                used += re.findall(r'\.num\s*=\s*GPIO_NUM_(\d+)', text)
                assert str(pin) not in used, (name, pin, used)
                if pin == 1:
                    source += '\n#ifdef RG_GPIO_SND_HP_DETECT\n#error Battery pin must not be configured as jack detect\n#endif\n'
            source += 'return 0;}\n'
            compile_run(args.cc, folder, name, source)
            count += 1

        inp = (ROOT / 'components/retro-go/rg_input.c').read_text()
        reader = inp[inp.index('bool rg_input_read_battery_raw('):inp.index('static uint32_t apply_virt_map(')]
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#define ADC_UNIT_1 1
#define ADC_CHANNEL_0 0
#define RG_BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#include "components/retro-go/targets/esp32-s3-battery.h"
#define RG_MAX(a,b) ((a)>(b)?(a):(b))
#define RG_MIN(a,b) ((a)<(b)?(a):(b))
typedef struct { float level, volts; bool present, charging; } rg_battery_t;
static int adc_chars, samples[4], position, conversions;
static int adc_get_raw(int unit,int channel) {
    assert(unit==1 && channel==0 && position<4); return samples[position++];
}
static uint32_t esp_adc_cal_raw_to_voltage(int raw,int *cal) {
    assert(cal==&adc_chars); ++conversions; return (uint32_t)raw;
}
''' + reader + r'''
static void samples_at(int mv) {
    position=conversions=0; for(int i=0;i<4;i++)samples[i]=mv;
}
int main(void) {
    rg_battery_t b;
    samples_at(2100); assert(rg_input_read_battery_raw(&b));
    assert(b.present && !b.charging && fabsf(b.volts-4.2f)<0.001f && fabsf(b.level-100.f)<0.001f && conversions==4);
    samples_at(1850); assert(rg_input_read_battery_raw(&b)); assert(fabsf(b.volts-3.7f)<0.001f && b.level>44.f && b.level<45.f);
    samples_at(1650); assert(rg_input_read_battery_raw(&b)); assert(b.present && b.level==0);
    samples_at(1600); assert(rg_input_read_battery_raw(&b)); assert(b.present && b.level==0);
    samples_at(2200); assert(rg_input_read_battery_raw(&b)); assert(b.level==100);
    samples_at(0); assert(rg_input_read_battery_raw(&b)); assert(!b.present && b.volts==0 && b.level==0);
    samples_at(999); assert(rg_input_read_battery_raw(&b)); assert(!b.present);
    samples_at(1800); samples[1]=samples[3]=1900; assert(rg_input_read_battery_raw(&b)); assert(fabsf(b.volts-3.7f)<0.001f);
    samples_at(-1); assert(!rg_input_read_battery_raw(&b)); assert(conversions==0);
    samples_at(1850); assert(rg_input_read_battery_raw(NULL));
    return 0;
}
'''
        compile_run(args.cc, folder, 'adc_reader', source)

        audio = (ROOT / 'components/retro-go/drivers/audio/i2s.c').read_text()
        reader = audio[audio.index('static bool read_jack('):audio.index('// The detect line is a mechanical')]
        route = audio[audio.index('static void resolve_route('):audio.index('#if RG_AUDIO_USE_HEADPHONE_JACK\nstatic bool read_jack(')]
        for detect in (False, True):
            source = r'''
#include <assert.h>
#include <stdbool.h>
#define DEVICE_EXT_HEADPHONES 1
#define DEVICE_EXT_AUTO 2
#define RG_AUDIO_ROUTE_HEADPHONES 1
#define RG_AUDIO_ROUTE_SPEAKER 0
static struct { int device,route; bool jack_present; } state;
'''
            if detect:
                source += r'''
#define RG_GPIO_SND_HP_DETECT 12
#define RG_GPIO_SND_HP_DETECT_LEVEL 0
static int level;
static int gpio_get_level(int pin) { assert(pin==12); return level; }
'''
            source += reader + route
            source += 'int main(void) {'
            if detect:
                source += 'level=0; assert(read_jack()); level=1; assert(!read_jack());'
            else:
                source += 'assert(!read_jack());'
            source += '''state.device=DEVICE_EXT_HEADPHONES; resolve_route(); assert(state.route==RG_AUDIO_ROUTE_HEADPHONES);
state.device=DEVICE_EXT_AUTO; state.jack_present=false; resolve_route(); assert(state.route==RG_AUDIO_ROUTE_SPEAKER);
return 0;}'''
            compile_run(args.cc, folder, 'audio_detect_' + str(detect), source)
    print(f'Battery enabled on all {count} ESP32-S3 targets; pin conflicts, divider math, presence, sampling/errors and manual headphone routing passed.')


if __name__ == '__main__':
    main()
