"""Build the pinned Nestopia core and exercise the real adapter's ROM/save IO.

Requires a native C++ compiler (GCC/Clang, or --cxx /path/to/zig).
Uses generated NROM/UxROM/CNROM/MMC3 ROMs; no copyrighted game data or hardware required.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import subprocess
import tempfile
import importlib.util
import hashlib

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "retro-core/components/nestopia/upstream/source/core"

PRELUDE = r'''
#include <NstApiEmulator.hpp>
#include <NstApiMachine.hpp>
#include <NstApiInput.hpp>
#include <NstApiSound.hpp>
#include <NstApiUser.hpp>
#include <NstApiVideo.hpp>
#include <rg_input.h>
#include <algorithm>
#ifdef NDEBUG
#undef NDEBUG
#endif
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

DIFFERENTIAL = r'''
// Exercise real background/sprite rendering, palette emphasis, NMI, OAM/DMC
// DMA, both controllers and all base audio channels with an original test ROM.
static void performance_regression() {
    uint64_t audioHash=14695981039346656037ULL, videoHash=audioHash, stateHash=audioHash;
    auto hash = [&](uint64_t &digest, const void *data, size_t size) {
        auto bytes = static_cast<const unsigned char *>(data);
        while (size--) { digest ^= *bytes++; digest *= 1099511628211ULL; }
    };
    for (unsigned mapper : {0U,2U,3U,4U}) for (bool pal : {false, true}) for (unsigned scene=0;scene<4;scene++) {
        const bool toggleRendering=scene==1;
        const unsigned prgSize=mapper?32768:16384, chrSize=mapper?16384:8192;
        std::string image(16 + prgSize + chrSize, '\0');
        memcpy(&image[0], "NES\x1a", 4); image[4]=prgSize/16384; image[5]=chrSize/8192;
        image[6]=(mapper<<4)|2; image[9]=pal;
        if(mapper) std::fill(image.begin()+16,image.begin()+16+prgSize,char(0xff));
        std::vector<unsigned char> code;
        auto emit = [&](std::initializer_list<unsigned char> bytes) { code.insert(code.end(),bytes); };
        auto imm = [&](unsigned op, unsigned value) { emit({(unsigned char)op,(unsigned char)value}); };
        auto absolute = [&](unsigned op, unsigned address) { emit({(unsigned char)op,(unsigned char)address,(unsigned char)(address>>8)}); };
        auto write = [&](unsigned address, unsigned value) { imm(0xa9,value); absolute(0x8d,address); };
        auto branch = [&](unsigned op, size_t label) { imm(op,unsigned(label-code.size()-2)); };
        emit({0x78,0xd8}); imm(0xa2,0xff); emit({0x9a});
        write(0x2000,0); write(0x2001,0); write(0x4017,0x40);
        size_t wait=code.size(); absolute(0x2c,0x2002); branch(0x10,wait);
        write(0x2006,0x3f); write(0x2006,0); imm(0xa2,0);
        size_t colors=code.size(); emit({0x8a}); absolute(0x8d,0x2007); emit({0xe8}); imm(0xe0,32); branch(0xd0,colors);
        write(0x2006,0x20); write(0x2006,0); imm(0xa0,4); imm(0xa2,0);
        size_t tiles=code.size(); emit({0x8a}); absolute(0x8d,0x2007); absolute(0x9d,0x0200); emit({0xe8}); branch(0xd0,tiles); emit({0x88}); branch(0xd0,tiles);
        if(scene>=2) {
            imm(0xa2,0); size_t objects=code.size();
            imm(0xa9,40); absolute(0x9d,0x0200);
            emit({0x8a}); absolute(0x9d,0x0201);
            emit({0x8a}); imm(0x49,0xa3); absolute(0x9d,0x0202);
            imm(0xa9,12); absolute(0x9d,0x0203);
            emit({0xe8,0xe8,0xe8,0xe8}); branch(0xd0,objects);
        }
        write(0x4015,0x1f); write(0x4000,0xbf); write(0x4002,0x80); write(0x4003,0x08);
        write(0x4004,0x7a); write(0x4006,0xc0); write(0x4007,0x08);
        write(0x4008,0xff); write(0x400a,0x40); write(0x400b,0x08);
        write(0x400c,0x3f); write(0x400e,0x06); write(0x400f,0x08);
        write(0x4010,0x4f); write(0x4012,0x00); write(0x4013,0x01); write(0x4015,0x1f);
        write(0x2005,0); write(0x2005,0); write(0x2000,0x80); write(0x2001,0x1e);
        unsigned loopAddress=0xc000+code.size();
        if(toggleRendering) {
            // Repeatedly interrupt visible pixels and sprite fetches. This
            // distinguishes running X counters from frozen sprite shifters.
            emit({0xe6,0x01,0xa5,0x01}); imm(0x29,0x1f); absolute(0x8d,0x2001);
            // Observe secondary-OAM clearing/evaluation between rendering
            // changes, accumulating every returned latch value into RAM.
            absolute(0xad,0x2004); absolute(0x4d,0x6002); absolute(0x8d,0x6002);
        }
        absolute(0x4c,loopAddress);
        unsigned nmi=0xc000+code.size();
        emit({0x48,0x8a,0x48,0xe6,0x00});
        write(0x2003,0); write(0x4014,2);
        emit({0xa5,0x00}); absolute(0x8d,0x2005); write(0x2005,0);
        emit({0xa5,0x00}); imm(0x29,0xe0); imm(0x09,0x1e); absolute(0x8d,0x2001);
        emit({0xa5,0x00}); absolute(0x8d,0x4002);
        if(mapper) {
            if(mapper==4) write(0x8000,6);
            emit({0xa5,0x00}); imm(0x29,1); absolute(0x8d,mapper==4?0x8001:0x8000);
            absolute(0xad,0x8000); absolute(0x8d,0x6004);
            if(mapper==4) { write(0xc000,17); write(0xc001,0); write(0xe001,0); }
        }
        write(0x4016,1); write(0x4016,0);
        absolute(0xad,0x4016); absolute(0x8d,0x6000); absolute(0xad,0x4017); absolute(0x8d,0x6001);
        emit({0x68,0xaa,0x68,0x40});
        memcpy(&image[16+prgSize-16384],code.data(),code.size());
        for(unsigned i=0;i<chrSize;i++) image[16+prgSize+i]=char((i*13)^(i>>3)^(i>>13)*0x5a);
        for(unsigned i=0;i<3;i++) { unsigned address=i==1?0xc000:nmi; image[16+prgSize-6+i*2]=char(address); image[16+prgSize-5+i*2]=char(address>>8); }
        Nes::Api::Emulator emulator; Nes::Api::Machine machine(emulator);
        Nes::Api::User::fileIoCallback.Unset();
        MemoryStreamBuffer buffer(image.data(),image.size()); std::istream input(&buffer);
        assert(machine.Load(input,Nes::Api::Machine::FAVORED_NES_NTSC)>=0);
        assert(machine.SetMode(machine.GetDesiredMode())>=0);
        Nes::Api::Video video(emulator); Nes::Api::Video::RenderState state;
        assert(video.EnableUnlimSprites(scene==3)>=0);
        state.width=256; state.height=240; state.bits.count=16;
        state.bits.mask.r=0xf800; state.bits.mask.g=0x07e0; state.bits.mask.b=0x001f;
        state.filter=Nes::Api::Video::RenderState::FILTER_NONE;
        assert(video.SetRenderState(state)>=0);
        assert(Nes::Api::Input(emulator).AutoSelectControllers()>=0);
        assert(Nes::Api::Sound(emulator).SetSampleRate(32000)>=0);
        assert(machine.Power(true)>=0);
        std::vector<uint16_t> pixels(256*240); int16_t pcm[641];
        Nes::Core::Input::Controllers pads;
        for(unsigned frame=0;frame<240;frame++) {
            assert(video.SetDecoder(Nes::Api::Video::Decoder(
                static_cast<Nes::Api::Video::DecoderPreset>((frame/48)%5)))>=0);
            pads.pad[0].buttons=frame&0xff; pads.pad[1].buttons=(frame^0x55)&0xff;
            Nes::Core::Sound::Output audio(pcm,641);
            Nes::Core::Video::Output output(pixels.data(),512);
#ifdef RG_TEST_DEFERRED_VIDEO
            assert(emulator.Execute(nullptr,&audio,&pads)>=0);
            if(frame%3) assert(video.Blit(output)>=0);
#else
            assert(emulator.Execute(frame%3?&output:nullptr,&audio,&pads)>=0);
#endif
            hash(audioHash,pcm,audio.length[0]*sizeof(pcm[0]));
            if(frame%3) hash(videoHash,pixels.data(),pixels.size()*sizeof(pixels[0]));
            if(frame==119) { std::stringstream saved; assert(machine.SaveState(saved,Nes::Api::Machine::NO_COMPRESSION)>=0); assert(machine.LoadState(saved)>=0); }
        }
        std::stringstream saved; assert(machine.SaveState(saved,Nes::Api::Machine::NO_COMPRESSION)>=0);
        std::string bytes=saved.str(); hash(stateHash,bytes.data(),bytes.size());
        std::string name=std::string(pal?"pal":"ntsc")+(scene==3?"-unlimited":scene==2?"-crowded":toggleRendering?"-toggle":"");
        if(mapper) name+="-mapper"+std::to_string(mapper);
        std::ofstream(name+".state",std::ios::binary).write(bytes.data(),bytes.size());
        std::ofstream(name+".video",std::ios::binary).write((const char*)pixels.data(),pixels.size()*2);
    }
    printf("Video/audio/state fingerprints: %016llx %016llx %016llx\n",
           (unsigned long long)videoHash,(unsigned long long)audioHash,(unsigned long long)stateHash);
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
    bool assertionsEnabled = false;
    assert(assertionsEnabled = true);
    if (!assertionsEnabled) std::abort();
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
        // A known baseline reproduces Linux's original checksum/save failure
        // on Windows too, rather than depending on power-on RAM contents.
        const std::string initialRam(8192, '\0');
        std::ofstream("test.sram.nestopia.battery",std::ios::binary).write(initialRam.data(),initialRam.size());
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
        saved.close();
        // Reload the cartridge so the saved pressed-state becomes its battery
        // checksum baseline. A power toggle alone keeps the original baseline;
        // returning RAM to that original value may intentionally skip saving.
        assert(machine.Unload()>=0);
        assert(load_image(machine,"test.zip")>=0);
        assert(Nes::Api::Input(emulator).AutoSelectControllers()>=0);
        assert(machine.Power(true)>=0);
        pads.pad[0].buttons=pads.pad[1].buttons=0;
        assert(emulator.Execute(nullptr,nullptr,&pads)>=0);
        assert(machine.Power(false)>=0);
        std::ifstream released("test.sram.nestopia.battery",std::ios::binary);
        assert(released.get()==0 && released.get()==0);
    }
    performance_regression();
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--upstream", action="store_true", help="Test the unmodified pinned core for differential comparison")
    parser.add_argument("--artifacts", type=Path, help="Keep the native executable and final states for diagnosis")
    parser.add_argument("--reference", type=Path, help="Compare all frame fingerprints with a prior --upstream --artifacts run")
    args = parser.parse_args()
    adapter = (ROOT / "retro-core/main/main_nestopia.cpp").read_text()
    stream_and_io = adapter[adapter.index("class MemoryStreamBuffer"):adapter.index("static void draw_nsf_overlay")]
    load = adapter[adapter.index("static Nes::Result load_image"):adapter.index("static void handle_nsf_input")]
    with tempfile.TemporaryDirectory() as directory:
        temp = args.artifacts.resolve() if args.artifacts else Path(directory)
        temp.mkdir(parents=True, exist_ok=True)
        core = CORE
        if not args.upstream:
            spec = importlib.util.spec_from_file_location("prepare_core", CORE.parents[2] / "prepare_core.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            core = module.prepare(CORE, temp / "core")
        source = temp / "test.cpp"
        buttons = adapter[adapter.index("static uint32_t convert_buttons"):adapter.index("static Nes::Result load_image")]
        deferred = "#define RG_TEST_DEFERRED_VIDEO 1\n" if not args.upstream else ""
        source.write_text(deferred + PRELUDE + stream_and_io + buttons + load + DIFFERENTIAL + RUNNER)
        compiler = str(Path(args.cxx).resolve()) if Path(args.cxx).exists() else args.cxx
        command = [compiler] + (["c++"] if Path(args.cxx).stem == "zig" else [])
        prefix = len(command)
        # Exercise the exact production options with the Xtensa 16-bit wchar_t
        # ABI. The normal Linux host compiler uses 32-bit wchar_t and otherwise
        # misses the firmware's NstCore.hpp narrowing failure.
        component = (CORE.parents[2] / "CMakeLists.txt").read_text()
        production_options = re.search(r"rg_setup_compile_options\((.*?)\)", component, re.S).group(1).split()
        wide_test = temp / "wide.cpp"
        wide_test.write_text('''#include "NstCore.hpp"
static_assert(sizeof(wchar_t)==2, "Test the Xtensa wchar_t ABI");
int main() {
    if (Nes::Core::StringCompare(L"mMc3", L"MMC3") != 0) return 1;
    if (Nes::Core::StringCompare(L"a", L"B") >= 0) return 2;
    if (Nes::Core::StringCompare(L"z", L"Y") <= 0) return 3;
    return 0;
}
''')
        wide_executable = temp / "wide.exe"
        subprocess.run(command + ["-std=c++11", "-fshort-wchar", "-Werror=narrowing"] + production_options +
                       ["-I" + str(core), str(wide_test), "-o", str(wide_executable)], check=True)
        subprocess.run([str(wide_executable)], check=True)
        command += ["-std=c++11", "-O1", "-DNST_NO_ZLIB", "-fno-rtti", "-Wno-narrowing", "-I" + str(CORE),
                    "-I" + str(core / "api"), "-I" + str(ROOT / "components/retro-go")]
        command = [arg.replace("-I" + str(CORE), "-I" + str(core)) for arg in command]
        sources = [source] + list(core.rglob("*.cpp"))
        objects = [temp / f"{index}.o" for index in range(len(sources))]
        headers = hashlib.sha256()
        for path in sorted(core.rglob('*.hpp')):
            headers.update(path.read_bytes())
        compile_key = headers.digest() + repr(command).encode()
        def compile_one(item):
            path, obj = item
            extra = b''.join(p.read_bytes() for p in sorted(core.glob('*.inc'))) if path.name == 'NstPpu.cpp' else b''
            signature = hashlib.sha256(compile_key + str(path).encode() + path.read_bytes() + extra).hexdigest()
            stamp = obj.with_suffix('.sha256')
            if args.artifacts and obj.exists() and stamp.exists() and stamp.read_text() == signature:
                return
            subprocess.run(command + ["-c", str(path), "-o", str(obj)], check=True)
            stamp.write_text(signature)
        # Bound compiler memory use on Windows and small CI workers.
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(compile_one, zip(sources, objects)))
        executable = temp / "nestopia-test.exe"
        command = command[:prefix] + [str(obj) for obj in objects] + ["-o", str(executable)]
        # A response file avoids Windows' command length limit.
        response = temp / "args.rsp"
        response.write_text("\n".join('"' + arg.replace('\\', '/') + '"' for arg in command[prefix:]))
        subprocess.run(command[:prefix] + ["@" + str(response)], check=True)
        run_dir = temp
        if args.artifacts:
            run_dir = args.artifacts.resolve()
            run_dir.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([str(executable)], cwd=run_dir, check=True, capture_output=True, text=True)
        print(result.stdout, end='')
        fingerprint = re.search(r'Video/audio/state fingerprints: ([0-9a-f ]+)', result.stdout).group(1).strip()
        if args.reference:
            assert fingerprint == (args.reference / 'fingerprints.txt').read_text().strip(), 'Upstream frame fingerprints differ'
            for path in run_dir.glob('*.state'):
                assert path.read_bytes() == (args.reference / path.name).read_bytes(), f'State differs: {path.name}'
            print('PASS: every rendered frame and audio sample, plus final save states, match upstream')
        if args.artifacts:
            (run_dir / 'fingerprints.txt').write_text(fingerprint)
    print("PASS: production 16-bit wchar_t options, ZIP/PAL/NTSC ROMs, 2400 audio guard frames, 7680 video/audio stress frames (NROM/UxROM/CNROM/MMC3), states/battery IO, independent NES ports")


if __name__ == "__main__":
    main()
