#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define RG_STORAGE_ROOT "sd"
#define RG_PATH_MAX 255
#define TRANSFER_SIZE 8192
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
typedef struct {const char *uri; size_t content_len;} httpd_req_t;
static struct {bool http_lock;} gui;
static char transfer_buffer[TRANSFER_SIZE], *http_buffer = transfer_buffer;
extern const unsigned char webui_gzip[];
extern const size_t webui_gzip_size;
static char served[131072], status[80], type[80];
static size_t served_size, receive_position, receive_length, fail_at;
static const char *receive_data;
static bool mounted = true, trusted = true, fail_send, allow_overwrite, fail_publish;
#define RG_LOGE(...) ((void)0)
#define RG_UPDATER_APPLICATION "factory"
#define RG_FIRMWARE_STAGE_PREPARE_UPDATE 1
#define RG_BOOT_ONCE 2
static int server=1, action_mutex;
static int settings_commits, restarts, starts, stops, installs, switches, last_volume, last_brightness, last_log;
static int64_t timer_now=1000000;
static bool confirm_install, valid_image=true;
static bool rg_mutex_take(int mutex,int timeout) {(void)mutex;(void)timeout;return true;}
static void rg_mutex_give(int mutex) {(void)mutex;}
static int64_t rg_system_timer(void) {return timer_now;}
static void rg_audio_set_volume(int value) {last_volume=value;}
static void rg_display_set_backlight(int value) {last_brightness=value;}
static void rg_system_set_log_level(int value) {last_log=value;}
static void rg_settings_commit(void) {settings_commits++;}
static void rg_storage_commit(void) {}
static void rg_system_restart(void) {restarts++;}
static void webui_stop(void) {stops++;server=0;}
static void webui_start(void) {starts++;server=1;}
static bool rg_gui_confirm(const char *title,const char *message,bool default_yes) {
    assert(!default_yes&&!strcmp(title,"Web firmware request")&&strstr(message,"sd/update.img"));return confirm_install;
}
static bool rg_firmware_install_image(const char *path,uint32_t flags) {
    assert(!strcmp(path,"sd/update.img")&&flags==RG_FIRMWARE_STAGE_PREPARE_UPDATE);installs++;return valid_image;
}
static void rg_system_switch_app(const char *app,const char *name,const char *path,uint32_t flags) {
    assert(!strcmp(app,"factory")&&!name&&!strcmp(path,"sd/update.img")&&flags==RG_BOOT_ONCE);switches++;
}
static int64_t available = 1000000;
static bool rg_storage_ready(void) {return mounted;}
static bool rg_storage_exists(const char *path) {struct stat s; return !stat(path, &s);}
typedef struct {bool exists,is_file;} rg_stat_t;
static rg_stat_t rg_storage_stat(const char *path) {
    struct stat s;bool exists=!stat(path,&s);return (rg_stat_t){exists,exists&&S_ISREG(s.st_mode)};
}
static int64_t rg_storage_get_free_space(const char *path) {(void)path; return available;}
static void rg_task_delay(int value) {(void)value;}
static void rg_task_yield(void) {}
static void gui_invalidate(void) {}
static int httpd_resp_set_status(httpd_req_t *req, const char *value) {(void)req; snprintf(status,sizeof(status),"%s",value);return 0;}
static int httpd_resp_set_type(httpd_req_t *req, const char *value) {(void)req;snprintf(type,sizeof(type),"%s",value);return 0;}
static int httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value) {(void)req;(void)key;(void)value;return 0;}
static int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *value, size_t length) {
    (void)req; assert(length>=2);
    bool enabled=!strcmp(key,"X-RetroGo")?trusted:allow_overwrite;
    snprintf(value,length,"%s",enabled?"1":"0"); return 0;
}
static int httpd_resp_send(httpd_req_t *req, const char *data, size_t length) {
    (void)req; if(fail_send) { return ESP_FAIL; } assert(length<sizeof(served));
    memcpy(served,data,length);served_size=length;served[length]=0;return ESP_OK;
}
static int httpd_resp_sendstr(httpd_req_t *req, const char *data) {return httpd_resp_send(req,data,strlen(data));}
static int httpd_resp_send_404(httpd_req_t *req) {httpd_resp_set_status(req,"404 Not Found");return 0;}
static int httpd_resp_send_chunk(httpd_req_t *req, const char *data, size_t length) {
    (void)req; if(fail_send) { return ESP_FAIL; } if(!data) { return ESP_OK; }
    assert(served_size+length<sizeof(served));memcpy(served+served_size,data,length);served_size+=length;return 0;
}
static int httpd_req_recv(httpd_req_t *req, char *buffer, size_t length) {
    (void)req;if(receive_position>=fail_at)return -1;
    size_t count=receive_length-receive_position;
    if(count>length) { count=length; }
    if(count>3) { count=3; }
    memcpy(buffer,receive_data+receive_position,count);receive_position+=count;return (int)count;
}
static int test_rename(const char *from,const char *to) {
    if(fail_publish && strstr(from,".rg-upload"))return -1;
    return rename(from,to);
}
#define rename test_rename
/* PRODUCTION_HANDLERS */
static void reset(const char *data) {
    receive_data=data;receive_length=strlen(data);receive_position=served_size=0;
    fail_at=SIZE_MAX;fail_send=false;status[0]=served[0]=0;assert(!gui.http_lock);
}
static void expect_file(const char *path, const char *value) {
    char data[200];FILE *fp=fopen(path,"rb");assert(fp);
    size_t n=fread(data,1,sizeof(data),fp);fclose(fp);assert(n==strlen(value)&&!memcmp(data,value,n));
}
int main(void) {
    char decoded[RG_PATH_MAX+1];
    assert(valid_path("sd",true) && !valid_path("sd",false));
    const char *bad[]={"sda/file","sd/../outside","sd/a/./b","sd//x","sd/a/","sd/.hidden","sd/file.","sd/file ","sd/a\\b","sd/a:b","sd/a.rg-upload"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!valid_path(bad[i],true));
    assert(request_path("sd/a%20b%23%25.txt?ignored=1",decoded)&&!strcmp(decoded,"sd/a b#%.txt"));
    const char *uris[]={"sd/%00x","sd/%2e%2e/x","sd/%","sd/%zz","sd/a%2fb/../c","sd/%5cfile"};
    for(size_t i=0;i<sizeof(uris)/sizeof(*uris);i++)assert(!request_path(uris[i],decoded));
    httpd_req_t req={"sd/new%20file.txt",12};
    reset("hello world!");assert(http_upload_handler(&req)==0);assert(!strcmp(served,"OK"));
    expect_file("sd/new file.txt","hello world!");assert(!rg_storage_exists("sd/new file.txt.rg-upload"));
    reset("replacement!");assert(http_upload_handler(&req)==0);assert(!strncmp(status,"409",3));expect_file("sd/new file.txt","hello world!");
    allow_overwrite=true;reset("replacement!");fail_at=3;http_upload_handler(&req);expect_file("sd/new file.txt","hello world!");
    reset("replacement!");fail_publish=true;http_upload_handler(&req);assert(!strncmp(status,"500",3));expect_file("sd/new file.txt","hello world!");fail_publish=false;
    reset("replacement!");assert(!http_upload_handler(&req));expect_file("sd/new file.txt","replacement!");assert(!rg_storage_exists("sd/new file.txt.rg-backup"));
    reset("hello world!");assert(!http_upload_handler(&req));allow_overwrite=false;
    req.uri="sd/aborted.txt";reset("hello world!");fail_at=3;http_upload_handler(&req);
    assert(!strncmp(status,"500",3)&&!gui.http_lock&&!rg_storage_exists("sd/aborted.txt")&&!rg_storage_exists("sd/aborted.txt.rg-upload"));
    req.uri="sd/denied.txt";reset("hello world!");trusted=false;http_upload_handler(&req);
    assert(!strncmp(status,"403",3)&&!rg_storage_exists("sd/denied.txt"));trusted=true;
    reset("hello world!");available=1;http_upload_handler(&req);assert(!strncmp(status,"507",3));available=1000000;
    reset("hello world!");mounted=false;http_upload_handler(&req);assert(!strncmp(status,"503",3));mounted=true;
    req.uri="sd/directory";reset("hello world!");allow_overwrite=true;http_upload_handler(&req);
    assert(!strncmp(status,"409",3)&&rg_storage_exists("sd/directory"));allow_overwrite=false;
    req.uri="sd/empty.txt";req.content_len=0;reset("");assert(!http_upload_handler(&req));expect_file("sd/empty.txt","");
    req.uri="sd/new%20file.txt";reset("");assert(!http_download_handler(&req));assert(served_size==12&&!memcmp(served,"hello world!",12)&&!gui.http_lock);
    reset("");fail_send=true;assert(http_download_handler(&req)==ESP_FAIL&&!gui.http_lock);
    req.uri="sd/missing.txt";reset("");http_download_handler(&req);assert(!strncmp(status,"404",3)&&!gui.http_lock);
    // Flash page works while storage is unavailable, without any file lookup.
    mounted=false;reset("");assert(!http_get_handler(&req)&&served_size==webui_gzip_size);
    assert(!memcmp(served,webui_gzip,webui_gzip_size)&&!strcmp(type,"text/html; charset=utf-8"));
    FILE *fp=fopen("served.gz","wb");assert(fp&&fwrite(served,1,served_size,fp)==served_size);fclose(fp);
    // Pending controls only run after the response delay and outside file transfers.
    pending.action=ACTION_VOLUME;pending.value=35;pending.at=timer_now+1;
    webui_poll();assert(!last_volume&&pending.action==ACTION_VOLUME);
    timer_now++;gui.http_lock=true;webui_poll();assert(!last_volume);gui.http_lock=false;
    webui_poll();assert(last_volume==35&&!pending.action&&settings_commits==1);
    pending.action=ACTION_BACKLIGHT;pending.value=60;webui_poll();assert(last_brightness==60);
    pending.action=ACTION_LOG;pending.value=4;webui_poll();assert(last_log==4);
    pending.action=ACTION_RESTART;webui_poll();assert(restarts==1);
    // Web requests cannot install until confirmation on the physical console.
    strcpy(pending.path,"sd/update.img");pending.action=ACTION_INSTALL;
    webui_poll();assert(!installs&&!switches&&stops==1&&starts==1&&server);
    confirm_install=true;valid_image=false;pending.action=ACTION_INSTALL;
    webui_poll();assert(installs==1&&!switches&&server);
    valid_image=true;pending.action=ACTION_INSTALL;
    webui_poll();assert(installs==2&&switches==1&&server);
    return 0;
}
