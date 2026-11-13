"""Run the real SD artwork lookup, SD-independent busy icon."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(path, start, end):
    source = (ROOT / path).read_text()
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    source = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#define RG_BASE_PATH "sd/retro-go"
#define RG_BASE_PATH_THEMES RG_BASE_PATH "/themes"
#define RG_PATH_MAX 255
struct {bool low_memory_mode,http_lock;} gui;
struct rg_image_t {int id;};
static rg_image_t selected_image={1}, default_image={2};
static const char *selected_theme;
static bool selected_exists, default_exists;
static int selected_reads, default_reads;
static std::string path_read;
static const char *rg_gui_get_theme_name() {return selected_theme;}
static rg_image_t *rg_gui_get_theme_image(const char *name) {
    selected_reads++; assert(!strcmp(name,"logo_gba.png"));
    return selected_exists?&selected_image:nullptr;
}
static rg_image_t *rg_surface_load_image_file(const char *path,int) {
    default_reads++; path_read=path; return default_exists?&default_image:nullptr;
}
'''
    source += section("launcher/main/gui.c", "rg_image_t *gui_get_image", "tab_t *gui_get_current_tab")
    source += r'''
#define RG_GUI_CENTER -100
#define C_BLACK 0
#define C_WHITE 65535
static uint16_t pixels[48*48];
static int rows_written;
static int get_horizontal_position(int pos,int width) {assert(pos==RG_GUI_CENTER);return (320-width)/2;}
static int get_vertical_position(int pos,int height) {assert(pos==RG_GUI_CENTER);return (240-height)/2;}
static void rg_display_write_rect(int x,int y,int w,int h,int stride,const uint16_t *data,uint32_t flags) {
    assert(x==136 && y==96+rows_written && w==48 && h==1 && stride==96 && flags==0);
    assert(rows_written<48); memcpy(pixels+48*rows_written++,data,96);
}
'''
    source += section("components/retro-go/rg_gui.c", "void rg_gui_draw_hourglass", "void rg_gui_draw_status_bars")
    source += r'''
int main() {
    selected_theme="custom"; selected_exists=default_exists=true;
    assert(gui_get_image("logo","gba")==&selected_image && !default_reads);
    selected_exists=false;
    assert(gui_get_image("logo","gba")==&default_image);
    assert(path_read==RG_BASE_PATH_THEMES "/default/logo_gba.png");
    selected_theme=nullptr;
    assert(gui_get_image("logo","gba")==&default_image);
    default_exists=false;
    assert(!gui_get_image("logo","gba"));
    selected_theme="default"; int before=default_reads;
    assert(!gui_get_image("logo","gba") && default_reads==before);
    gui.low_memory_mode=true; before=selected_reads;
    assert(!gui_get_image("logo","gba") && before==selected_reads);
    gui.low_memory_mode=false;
    // Neither a GUI framebuffer nor an SD card is available to this indicator.
    rg_gui_draw_hourglass(); assert(rows_written==48);
    int black=0; for(auto pixel:pixels) black+=pixel==C_BLACK;
    assert(black>200 && black<1500 && pixels[0]==C_WHITE);
}
'''
    compiler = str(Path(args.cxx).resolve()) if Path(args.cxx).exists() else args.cxx
    command = [compiler] + (["c++"] if Path(compiler).stem == "zig" else [])
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        cpp = folder / "sd_assets.cpp"
        cpp.write_text(source)
        executable = folder / "sd_assets.exe"
        subprocess.run(command + ["-std=c++11", "-O1", str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], cwd=folder, check=True)
    print("PASS: SD theme/default/missing-card lookups, procedural busy icon")


if __name__ == "__main__":
    main()
