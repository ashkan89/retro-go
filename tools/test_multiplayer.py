"""Execute controller routing and frontend mappings with independent P1/P2 states."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(path, start, end):
    text = (ROOT / path).read_text()
    return text[text.index(start):text.index(end, text.index(start))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    source = r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <string>
#include "rg_input.h"
#include "libretro.h"
static uint32_t states[2];
uint32_t rg_input_read_gamepad(void) { return states[0]; }
uint32_t rg_input_read_gamepad_player(rg_player_t p) { return states[p]; }
static bool primary, legacy;
#define RG_BASE_PATH_BIOS "/retro-go/bios"
static bool rg_storage_exists(const char *path) {
    return std::string(path).find("disksys.rom") != std::string::npos ? primary : legacy;
}
'''
    source += section("retro-core/main/shared.h", "static inline const char *nes_fds_bios_path", "extern uint8_t")
    source += r'''
namespace routing {
static int8_t source_assignment[RG_INPUT_SOURCE_COUNT], source_resolved_player[RG_INPUT_SOURCE_COUNT];
static uint16_t source_connect_seq[RG_INPUT_SOURCE_COUNT], connect_seq_counter;
static bool source_was_connected[RG_INPUT_SOURCE_COUNT], connected_now[RG_INPUT_SOURCE_COUNT];
static bool source_connected(rg_input_source_t s) { return connected_now[s]; }
'''
    source += section("components/retro-go/rg_input.c", "static void update_source_resolution", "bool rg_input_read_gamepad_raw")
    source += r'''
static void check() {
    for (int s=0;s<RG_INPUT_SOURCE_COUNT;s++) source_assignment[s]=RG_INPUT_PLAYER_AUTO;
    connected_now[0]=true; connected_now[1]=true;
    update_source_resolution();
    assert(source_resolved_player[0]==RG_PLAYER_1 && source_resolved_player[1]==RG_PLAYER_2);
    connected_now[2]=true; source_assignment[2]=RG_PLAYER_2;
    update_source_resolution();
    assert(source_resolved_player[1]==RG_INPUT_PLAYER_OFF && source_resolved_player[2]==RG_PLAYER_2);
    connected_now[2]=false; update_source_resolution();
    assert(source_resolved_player[1]==RG_PLAYER_2);
    source_assignment[0]=RG_INPUT_PLAYER_OFF; source_assignment[1]=RG_PLAYER_1;
    update_source_resolution(); assert(source_resolved_player[0]==RG_INPUT_PLAYER_OFF);
    connected_now[1]=false; update_source_resolution();
    assert(source_resolved_player[0]==RG_PLAYER_1); // retain usable controls after unplug
}
}
namespace a78 {
static unsigned char input[17];
static void map(uint32_t keys) {
'''
    source += section("retro-legacy/main/prosystem.c", "    input[0] =", "    // Carry the fractional")
    source += r'''
}
static void check() {
    states[1]=RG_KEY_LEFT|RG_KEY_B; map(RG_KEY_UP|RG_KEY_A);
    assert(input[3] && input[5] && !input[1] && !input[4]);
    assert(input[7] && input[10] && !input[9] && !input[11]);
    states[1]=0; map(0); for(int i=0;i<15;i++) assert(!input[i]);
}
}
namespace a26 {
struct Event {
    enum {JoystickZeroUp,JoystickZeroDown,JoystickZeroLeft,JoystickZeroRight,JoystickZeroFire,
          JoystickOneUp,JoystickOneDown,JoystickOneLeft,JoystickOneRight,JoystickOneFire,
          ConsoleSelect,ConsoleReset};
    bool values[12] = {}; void set(int i, bool value) { values[i]=value; }
};
static Event event;
static void map(uint32_t keys) {
'''
    source += section("retro-legacy/main/stella.cpp", "    event.set(Event::JoystickZeroUp", "    console->controller")
    source += r'''
}
static void check() {
    states[1]=RG_KEY_LEFT|RG_KEY_B; map(RG_KEY_UP|RG_KEY_A);
    assert(event.values[Event::JoystickZeroUp] && !event.values[Event::JoystickZeroLeft]);
    assert(event.values[Event::JoystickOneLeft] && !event.values[Event::JoystickOneUp]);
    assert(event.values[Event::JoystickOneFire]);
    states[1]=0; map(0); for(bool value:event.values) assert(!value);
}
}
namespace spectrum {
static int controls; static unsigned z80_inports[256];
'''
    source += section("retro-legacy/main/spectrum.c", "static void press", "static void step")
    source += "static void map(uint32_t keys, uint8_t rows[8]) {\n"
    source += section("retro-legacy/main/spectrum.c", "    unsigned joystick =", "    if (caps_shift)")
    source += r'''
}
static void check() {
    for(controls=0;controls<3;controls++) {
        uint8_t rows[8]; memset(rows,255,8); states[1]=RG_KEY_LEFT|RG_KEY_A;
        map(0,rows); assert(rows[3]==(255 & ~1 & ~16)); assert(z80_inports[0x1f]==0);
        memset(rows,255,8); states[1]=0; map(0,rows);
        for(int i=0;i<8;i++) assert(rows[i]==255);
    }
}
}
namespace sms {
enum {INPUT_UP=1,INPUT_DOWN=2,INPUT_LEFT=4,INPUT_RIGHT=8,INPUT_BUTTON1=16,INPUT_BUTTON2=32};
static struct {unsigned pad[2],system;} input;
static struct {unsigned keypad[2];} coleco;
static int colecoKeypadPlayer, colecoKeyPlayer, colecoKey=7, colecoKeyDecay;
typedef int rg_gui_event_t;
struct rg_gui_option_t {char *value;};
enum {RG_DIALOG_PREV,RG_DIALOG_NEXT,RG_DIALOG_VOID};
#define NS_APP "test"
static void rg_settings_set_number(const char *,const char *,int) {}
'''
    source += section("retro-core/main/main_sms.c", "static rg_gui_event_t keypad_player_cb", "static void options_handler")
    source += r'''
static void keypad() {
    coleco.keypad[0]=coleco.keypad[1]=255;
'''
    source += section("retro-core/main/main_sms.c", "            if (colecoKeyDecay > 0)", "            if (joystick & RG_KEY_START)")
    source += r'''
}
static void map(uint32_t joystick) {
'''
    source += section("retro-core/main/main_sms.c", "        uint32_t joystick2 =", "        if (IS_SMS)")
    source += r'''
}
static void check() {
    states[1]=RG_KEY_LEFT|RG_KEY_B; map(RG_KEY_UP|RG_KEY_A);
    assert(input.pad[0]==(INPUT_UP|INPUT_BUTTON2)); assert(input.pad[1]==(INPUT_LEFT|INPUT_BUTTON1));
    states[1]=0; map(0); assert(!input.pad[0] && !input.pad[1]);
    char value[32]; rg_gui_option_t opt={value};
    keypad_player_cb(&opt,RG_DIALOG_NEXT); assert(colecoKeypadPlayer==1 && !strcmp(value,"P2"));
    colecoKeyPlayer=colecoKeypadPlayer; colecoKeyDecay=1; keypad();
    assert(coleco.keypad[0]==255 && coleco.keypad[1]==7);
    keypad(); assert(coleco.keypad[0]==255 && coleco.keypad[1]==255);
    keypad_player_cb(&opt,RG_DIALOG_PREV); assert(!colecoKeypadPlayer && !strcmp(value,"P1"));
}
}
namespace snes {
#define RG_COUNT(a) (sizeof(a)/sizeof((a)[0]))
static struct {struct {uint32_t local_mask,mod_mask,snes9x_mask;} keys[2];} keymap = {{{RG_KEY_A,0,1},{RG_KEY_LEFT,0,2}}};
'''
    source += section("retro-core/main/main_snes.c", "uint32_t S9xReadJoypad", "bool S9xReadMousePosition")
    source += r'''
static void check() {
    states[0]=RG_KEY_A; states[1]=RG_KEY_LEFT;
    assert(S9xReadJoypad(0)==1 && S9xReadJoypad(1)==2 && S9xReadJoypad(2)==0);
    states[0]=states[1]=0; assert(!S9xReadJoypad(0) && !S9xReadJoypad(1));
}
}
'''
    source += r'''
namespace pce {
enum {JOY_UP=1,JOY_DOWN=2,JOY_LEFT=4,JOY_RIGHT=8,JOY_A=16,JOY_B=32,JOY_RUN=64,JOY_SELECT=128};
static bool emulationPaused;
static void rg_gui_game_menu() {} static void rg_gui_options_menu() {}
'''
    source += section("retro-core/main/main_pce.c", "void osd_input_read", "static void audioTask")
    source += r'''
static void check() {
    uint8_t pads[8]={}; states[0]=RG_KEY_UP|RG_KEY_A; states[1]=RG_KEY_LEFT|RG_KEY_B;
    osd_input_read(pads); assert(pads[0]==(JOY_UP|JOY_A) && pads[1]==(JOY_LEFT|JOY_B));
    states[0]=states[1]=0; osd_input_read(pads); assert(!pads[0] && !pads[1]);
}
}
namespace msx {
enum {JST_UP=1,JST_DOWN=2,JST_LEFT=4,JST_RIGHT=8,JST_FIREA=16,JST_FIREB=32};
static int JoyState;
static void map(uint32_t joystick) { JoyState=0; if(false) {}
'''
    source += section("fmsx/main/main.c", "    else\n    {\n        if (joystick & RG_KEY_LEFT)\n            JoyState", "    return 0;")
    source += r'''
}
static void check() {
    states[1]=RG_KEY_LEFT|RG_KEY_B; map(RG_KEY_UP|RG_KEY_A);
    assert((JoyState&255)==(JST_UP|JST_FIREA) && (JoyState>>8)==(JST_LEFT|JST_FIREB));
    states[1]=0; map(0); assert(!JoyState);
}
}
namespace nes {
enum {NES_PAD_UP=1,NES_PAD_DOWN=2,NES_PAD_LEFT=4,NES_PAD_RIGHT=8,NES_PAD_A=16,NES_PAD_B=32,NES_PAD_START=64,NES_PAD_SELECT=128};
static int pads[2];
static void map(uint32_t joystick) {
'''
    source += section("retro-core/main/main_nes.c", "        int buttons = 0;", "        if (drawFrame)")
    source += r'''
pads[0]=buttons; pads[1]=buttons2;
}
static void check() {
    states[1]=RG_KEY_LEFT|RG_KEY_B; map(RG_KEY_UP|RG_KEY_A);
    assert(pads[0]==(NES_PAD_UP|NES_PAD_A) && pads[1]==(NES_PAD_LEFT|NES_PAD_B));
    states[1]=0; map(0); assert(!pads[0] && !pads[1]);
}
}
namespace mega {
static unsigned pads[2];
static void gwenesis_io_pad_press_button(int p,int i) { pads[p]|=1<<i; }
static void gwenesis_io_pad_release_button(int p,int i) { pads[p]&=~(1<<i); }
static void map(uint32_t joystick, uint32_t joystick2) {
    uint32_t joystick_old=~joystick,joystick2_old=~joystick2;
    uint32_t keymap[8]={RG_KEY_UP,RG_KEY_DOWN,RG_KEY_LEFT,RG_KEY_RIGHT,RG_KEY_A,RG_KEY_B,RG_KEY_SELECT,RG_KEY_START};
'''
    source += section("gwenesis/main/main.c", "            if (joystick != joystick_old)", "        }\n\n        int64_t startTime")
    source += r'''
}
static void check() {
    map(RG_KEY_UP|RG_KEY_A,RG_KEY_LEFT|RG_KEY_B);
    assert(pads[0]==(1|16) && pads[1]==(4|32));
    map(0,0); assert(!pads[0] && !pads[1]);
}
}
'''
    source += "namespace gba { static uint32_t keys;\n"
    source += section("gbsp/main/main.c", "static int16_t input_state", "static bool save_sram")
    source += r'''
static void check() {
    const uint32_t keys_to_test[]={RG_KEY_B,RG_KEY_Y,RG_KEY_SELECT,RG_KEY_START,RG_KEY_UP,
        RG_KEY_DOWN,RG_KEY_LEFT,RG_KEY_RIGHT,RG_KEY_A,RG_KEY_X,RG_KEY_L,RG_KEY_R};
    for(unsigned id=0;id<12;id++) {
        keys=keys_to_test[id];
        for(unsigned other=0;other<12;other++)
            assert(input_state(0,RETRO_DEVICE_JOYPAD,0,other)==(id==other));
        assert(!input_state(1,RETRO_DEVICE_JOYPAD,0,id));
        assert(!input_state(0,RETRO_DEVICE_JOYPAD,1,id));
        assert(!input_state(0,RETRO_DEVICE_MOUSE,0,id));
    }
    keys=0; for(unsigned id=0;id<16;id++) assert(!input_state(0,RETRO_DEVICE_JOYPAD,0,id));
}
}
int main() {
    primary=legacy=false; assert(std::string(nes_fds_bios_path()).find("disksys.rom")!=std::string::npos);
    legacy=true; assert(std::string(nes_fds_bios_path()).find("fds_bios.bin")!=std::string::npos);
    primary=true; assert(std::string(nes_fds_bios_path()).find("disksys.rom")!=std::string::npos);
    routing::check(); a78::check(); a26::check(); spectrum::check(); sms::check(); snes::check();
    pce::check(); msx::check(); nes::check(); mega::check();
    gba::check();
}
'''
    compiler = str(Path(args.cxx).resolve()) if Path(args.cxx).exists() else args.cxx
    command = [compiler] + (["c++"] if Path(compiler).stem == "zig" else [])
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        cpp = folder / "multiplayer.cpp"
        cpp.write_text(source)
        executable = folder / "multiplayer.exe"
        subprocess.run(command + ["-std=c++11", "-O1", "-I", str(ROOT / "components/retro-go"),
                                  "-I", str(ROOT / "gbsp/components/gpsp/core/libretro/libretro-common/include"),
                                  str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: FDS BIOS precedence, controller assignment/unplug, independent P1/P2 mappings and release")


if __name__ == "__main__":
    main()
