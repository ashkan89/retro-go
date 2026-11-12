"""Build the pinned Nestopia core and exercise the real adapter's ROM/save IO.

Requires a native C++ compiler (GCC/Clang, or --cxx /path/to/zig).
Uses generated mapper-0 ROMs; no copyrighted game data or hardware required.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "retro-core/components/nestopia/upstream/source/core"

PRELUDE = r'''
#include <NstApiEmulator.hpp>
#include <NstApiMachine.hpp>
#include <NstApiInput.hpp>
#include <NstApiSound.hpp>
#include <NstApiUser.hpp>
#include <rg_input.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
struct { const char *romPath; } appStorage = {"test"}, *app = &appStorage;
#define RG_PATH_SAVE_SRAM 0
#define RG_FILE_ALIGN_8KB 0
#define RG_LOGE(...) ((void)0)
#define RG_PANIC(msg) throw std::runtime_error(msg)
static char *rg_emu_get_path(int, const char *) { return strdup("test.sram"); }
static const char *rg_dirname(const char *) { return "."; }
static bool rg_storage_mkdir(const char *) { return true; }
static bool rg_extension_match(const char *path, const char *) {
    return std::string(path).find(".zip") != std::string::npos;
}
static std::string zipped;
static bool rg_storage_unzip_file(const char *, const char *, void **data, size_t *size, int) {
    *size = zipped.size(); *data = malloc(*size); memcpy(*data, zipped.data(), *size); return true;
}
'''

RUNNER = r'''
static std::string rom(bool pal) {
    std::string data(16 + 16384 + 8192, '\0');
    memcpy(&data[0], "NES\x1a", 4);
    data[4] = 1; data[5] = 1; data[6] = 2; data[9] = pal;
    // Write a battery byte and loop forever. All interrupt vectors point here.
    const unsigned char program[] = {0x78,0xa9,0x5a,0x8d,0x00,0x60,0x4c,0x06,0xc0};
    memcpy(&data[16], program, sizeof(program));
    for (int i = 0; i < 3; ++i) { data[16+16384-6+i*2] = 0; data[16+16384-5+i*2] = char(0xc0); }
    return data;
}
int main() {
    char bytes[] = "abcdef";
    MemoryStreamBuffer buffer(bytes, 6); std::istream input(&buffer);
    input.seekg(-2, std::ios::end); assert(input.tellg() == 4); assert(input.get() == 'e');
    input.seekg(-3, std::ios::cur); assert(input.get() == 'c');
    input.seekg(7); assert(input.fail()); input.clear(); input.seekg(0); assert(input.get() == 'a');
    for (bool pal : {false, true}) for (bool zip : {false, true}) {
        std::remove("test.sram.nestopia.battery");
        zipped = rom(pal);
        std::ofstream("test.nes", std::ios::binary).write(zipped.data(), zipped.size());
        Nes::Api::Emulator emulator; Nes::Api::Machine machine(emulator);
        Nes::Api::User::fileIoCallback.Set(file_io, nullptr);
        assert(load_image(machine, zip ? "test.zip" : "test.nes") >= 0);
        assert(machine.GetDesiredMode() == (pal ? Nes::Api::Machine::PAL : Nes::Api::Machine::NTSC));
        assert(machine.SetMode(machine.GetDesiredMode()) >= 0);
        Nes::Api::Sound sound(emulator); assert(sound.SetSampleRate(32000) >= 0);
        assert(machine.Power(true) >= 0);
        Nes::Core::Input::Controllers pads;
        unsigned total = 0;
        for (int frame = 0; frame < 600; ++frame) {
            int16_t samples[643]; std::fill(samples, samples+643, 12345);
            Nes::Core::Sound::Output output(samples+1, 641);
            assert(emulator.Execute(nullptr, &output, &pads) >= 0);
            assert(output.length[0] <= 641 && samples[0] == 12345 && samples[642] == 12345);
            total += output.length[0];
        }
        // Physical frame frequencies are ~60.10 / 50.007 Hz.
        const unsigned expected = pal ? 383950 : 319475;
        assert(total > expected-1500 && total < expected+1500);
        std::stringstream state;
        assert(machine.SaveState(state, Nes::Api::Machine::NO_COMPRESSION) >= 0);
        assert(machine.LoadState(state) >= 0);
        assert(machine.Power(false) >= 0);
        std::ifstream saved("test.sram.nestopia.battery", std::ios::binary);
        assert(saved && saved.get() == 0x5a);
        // Re-load a fresh machine to prove adapter callbacks restore SRAM.
        assert(machine.Unload() >= 0);
        const unsigned char readback[] = {0x78,0xad,0x00,0x60,0x8d,0x01,0x60,0x4c,0x07,0xc0};
        memcpy(&zipped[16], readback, sizeof(readback));
        std::ofstream("test.nes", std::ios::binary).write(zipped.data(), zipped.size());
        assert(load_image(machine, "test.nes") >= 0);
        assert(machine.Power(true) >= 0);
        assert(emulator.Execute(nullptr, nullptr, &pads) >= 0);
        assert(machine.Power(false) >= 0);
        std::ifstream restored("test.sram.nestopia.battery", std::ios::binary);
        assert(restored.get() == 0x5a && restored.get() == 0x5a);
        assert(machine.Unload() >= 0);
    }
    // Read both hardware NES ports into battery RAM using a generated ROM.
    for (int active=0; active<2; ++active) {
        std::remove("test.sram.nestopia.battery");
        zipped=rom(false);
        const unsigned char program[] = {
            0x78,0xa9,1,0x8d,0x16,0x40,0xa9,0,0x8d,0x16,0x40,
            0xad,0x16,0x40,0x29,1,0x8d,0,0x60,
            0xad,0x17,0x40,0x29,1,0x8d,1,0x60,0x4c,1,0xc0};
        memcpy(&zipped[16],program,sizeof(program));
        Nes::Api::Emulator emulator; Nes::Api::Machine machine(emulator);
        assert(load_image(machine,"test.zip")>=0);
        assert(Nes::Api::Input(emulator).AutoSelectControllers()>=0);
        assert(machine.Power(true)>=0);
        Nes::Core::Input::Controllers pads;
        pads.pad[active].buttons=convert_buttons(RG_KEY_A);
        pads.pad[1-active].buttons=convert_buttons(RG_KEY_B);
        assert(emulator.Execute(nullptr,nullptr,&pads)>=0);
        assert(machine.Power(false)>=0);
        std::ifstream saved("test.sram.nestopia.battery",std::ios::binary);
        assert(saved.get()==(active==0) && saved.get()==(active==1));
        assert(machine.Power(true)>=0);
        pads.pad[0].buttons=pads.pad[1].buttons=0;
        assert(emulator.Execute(nullptr,nullptr,&pads)>=0);
        assert(machine.Power(false)>=0);
        std::ifstream released("test.sram.nestopia.battery",std::ios::binary);
        assert(released.get()==0 && released.get()==0);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    adapter = (ROOT / "retro-core/main/main_nestopia.cpp").read_text()
    stream_and_io = adapter[adapter.index("class MemoryStreamBuffer"):adapter.index("static void draw_nsf_overlay")]
    load = adapter[adapter.index("static Nes::Result load_image"):adapter.index("static void handle_nsf_input")]
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        source = temp / "test.cpp"
        buttons = adapter[adapter.index("static uint32_t convert_buttons"):adapter.index("static Nes::Result load_image")]
        source.write_text(PRELUDE + stream_and_io + buttons + load + RUNNER)
        compiler = str(Path(args.cxx).resolve()) if Path(args.cxx).exists() else args.cxx
        command = [compiler] + (["c++"] if Path(args.cxx).stem == "zig" else [])
        prefix = len(command)
        command += ["-std=c++11", "-O1", "-DNST_NO_ZLIB", "-fno-rtti", "-Wno-narrowing", "-I" + str(CORE),
                    "-I" + str(CORE / "api"), "-I" + str(ROOT / "components/retro-go")]
        sources = [source] + list(CORE.rglob("*.cpp"))
        objects = [temp / f"{index}.o" for index in range(len(sources))]
        def compile_one(item):
            path, obj = item
            subprocess.run(command + ["-c", str(path), "-o", str(obj)], check=True)
        # Bound compiler memory use on Windows and small CI workers.
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(compile_one, zip(sources, objects)))
        executable = temp / "nestopia-test.exe"
        command = command[:prefix] + [str(obj) for obj in objects] + ["-o", str(executable)]
        # A response file avoids Windows' command length limit.
        response = temp / "args.rsp"
        response.write_text("\n".join('"' + arg.replace('\\', '/') + '"' for arg in command[prefix:]))
        subprocess.run(command[:prefix] + ["@" + str(response)], check=True)
        subprocess.run([str(executable)], cwd=temp, check=True)
    print("PASS: ZIP/PAL/NTSC ROMs, 2400 audio frames, states/battery IO, independent NES $4016/$4017 controller ports and release")


if __name__ == "__main__":
    main()
