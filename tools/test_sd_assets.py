"""Run the real SD artwork lookup, web-UI streaming and SD-independent busy icon."""
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
typedef int esp_err_t;
struct httpd_req_t {const char *uri; size_t content_len;};
#define ESP_OK 0
#define ESP_FAIL -1
static char storage_buffer[65536], *http_buffer=storage_buffer;
static std::string served, content_type;
static int chunks;
static bool finished, fail_send;
static std::string upload;
static size_t upload_position;
#define RG_LOGI(...) ((void)0)
#define RG_LOGE(...) ((void)0)
static char *urldecode(const char *path) {return strdup(path);}
static void gui_invalidate() {}
static void rg_task_delay(int) {}
static int httpd_req_recv(httpd_req_t *,char *buffer,size_t size) {
    size_t count=upload.size()-upload_position;
    if(count>size) count=size;
    memcpy(buffer,upload.data()+upload_position,count); upload_position+=count;
    return count;
}
static void rg_task_yield() {}
static int httpd_resp_set_type(httpd_req_t *,const char *type) {content_type=type;return ESP_OK;}
static int httpd_resp_set_hdr(httpd_req_t *,const char *key,const char *value) {
    assert(!strcmp(key,"Cache-Control") && !strcmp(value,"no-store")); return ESP_OK;
}
static int httpd_resp_sendstr(httpd_req_t *,const char *data) {served=data;return ESP_OK;}
static int httpd_resp_send_chunk(httpd_req_t *,const char *data,size_t size) {
    if(fail_send) return ESP_FAIL;
    if(!data) {finished=true;return ESP_OK;}
    chunks++; served.append(data,size); return ESP_OK;
}
'''
    source += section("launcher/main/webui.c", "static const char webui_bootstrap", "static httpd_handle_t server")
    source += section("launcher/main/webui.c", "static esp_err_t http_upload_handler", "static esp_err_t http_download_handler")
    source += section("launcher/main/webui.c", "static esp_err_t http_get_handler", "void webui_stop")
    source += r'''
static void reset_http() {served.clear();chunks=0;finished=fail_send=false;gui.http_lock=false;}
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
    httpd_req_t request={RG_BASE_PATH "/webui/index.html",0};
    reset_http(); assert(http_get_handler(&request)==ESP_OK);
    assert(served.find("Install interface")!=std::string::npos && !gui.http_lock);
    assert(content_type=="text/html; charset=utf-8");
    const std::string full(70000,'x');
    std::ofstream(RG_BASE_PATH "/webui/index.html",std::ios::binary).write(full.data(),full.size());
    reset_http(); assert(http_get_handler(&request)==ESP_OK);
    assert(served==full && chunks==3 && finished && !gui.http_lock);
    reset_http(); fail_send=true; assert(http_get_handler(&request)==ESP_FAIL);
    assert(!gui.http_lock && !finished);
    std::ofstream(RG_BASE_PATH "/webui/index.html",std::ios::binary|std::ios::trunc).close();
    reset_http(); assert(http_get_handler(&request)==ESP_OK);
    assert(served.find("Install interface")!=std::string::npos && !gui.http_lock);
    std::remove(RG_BASE_PATH "/webui/index.html");
    upload="<!doctype html>Installed SD interface"; upload_position=0;
    request.content_len=upload.size();
    reset_http(); assert(http_upload_handler(&request)==ESP_OK);
    assert(served=="OK" && !gui.http_lock);
    reset_http(); assert(http_get_handler(&request)==ESP_OK && served==upload);
    upload_position=0; request.content_len=upload.size()+1;
    reset_http(); assert(http_upload_handler(&request)==ESP_FAIL);
    assert(served=="ERROR" && !gui.http_lock);
    assert(std::rename("sd","unmounted-sd")==0);
    upload_position=0; request.content_len=upload.size();
    reset_http(); assert(http_upload_handler(&request)==ESP_FAIL);
    assert(served=="ERROR" && !gui.http_lock);
    reset_http(); assert(http_get_handler(&request)==ESP_OK);
    assert(served.find("Install interface")!=std::string::npos && !gui.http_lock);
}
'''
    compiler = str(Path(args.cxx).resolve()) if Path(args.cxx).exists() else args.cxx
    command = [compiler] + (["c++"] if Path(compiler).stem == "zig" else [])
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        (folder / "sd/retro-go/webui").mkdir(parents=True)
        cpp = folder / "sd_assets.cpp"
        cpp.write_text(source)
        executable = folder / "sd_assets.exe"
        subprocess.run(command + ["-std=c++11", "-O1", str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], cwd=folder, check=True)
    print("PASS: SD theme/default/missing-card lookups, procedural busy icon, web UI streaming/recovery/disconnect handling")


if __name__ == "__main__":
    main()
