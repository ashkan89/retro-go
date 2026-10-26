#!/usr/bin/env python3
"""Fault injection for the production SD transaction, file, scan, and firmware CRC code."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STORAGE = (ROOT / 'components/retro-go/rg_storage.c').read_text()
FIRMWARE = (ROOT / 'components/retro-go/rg_firmware.c').read_text()
APPS = (ROOT / 'launcher/main/applications.c').read_text()


def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def run_c(cc, folder, name, source, *args):
    path = folder / (name + '.c')
    exe = folder / name
    path.write_text(source)
    subprocess.run([cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-I', str(ROOT / 'components/retro-go'), str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe), *map(str, args)], check=True)


BASE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define RG_MIN(a,b) ((a)<(b)?(a):(b))
#define RG_MAX(a,b) ((a)>(b)?(a):(b))
#define RG_LOGE(...) ((void)0)
#define RG_LOGW(...) ((void)0)
#define RG_LOGI(...) ((void)0)
#define RG_LOGD(...) ((void)0)
#define RG_ASSERT_ARG(x) assert(x)
#define _(x) (x)
static void rg_task_delay(int ms) { (void)ms; }
'''


def test_files(cc, folder):
    source = BASE + r'''
#define RG_PATH_MAX 255
#define RG_STORAGE_ROOT "/sd"
#include "rg_storage.h"
#define FILE_IO_RETRIES 3
#define FILE_IO_CHUNK_SIZE 16384
#define CHECK_PATH(p) if (!(p) || !*(p)) return false
static int read_failures, bad_reads, poison, opens, close_failures, write_failures;
static int directory_failures, stat_failures, unknown_type, nested_failures, publish_failures;
static FILE *fault_open(const char *p, const char *m) { ++opens; poison=0; return fopen(p,m); }
static size_t fault_read(void *p,size_t a,size_t b,FILE *fp) {
    if (poison || read_failures) {
        if (read_failures>0) --read_failures;
        poison=1; errno=EIO; return 0;
    }
    size_t n=fread(p,a,b,fp);
    if (n && bad_reads) { if (bad_reads>0) --bad_reads; ((uint8_t*)p)[0]^=1; }
    return n;
}
static size_t fault_write(const void *p,size_t a,size_t b,FILE *fp) {
    if (write_failures) { if(write_failures>0)--write_failures; errno=EIO; return 0; }
    return fwrite(p,a,b,fp);
}
static int fault_close(FILE *fp) {
    int ret=fclose(fp); poison=0;
    if(close_failures) { --close_failures; errno=EIO; return EOF; } return ret;
}
static struct dirent *fault_readdir(DIR *d) {
    struct dirent *e=readdir(d);
    if(e && !strcmp(e->d_name,"nested-failure.bin") && nested_failures) { errno=EIO; return NULL; }
    if(e && !strcmp(e->d_name,"b.bin") && directory_failures) {
        if(directory_failures>0) { --directory_failures; }
        errno=EIO; return NULL;
    }
    if(e && unknown_type && !strcmp(e->d_name,"b.bin")) e->d_type=DT_UNKNOWN;
    return e;
}
static int fault_stat(const char *p,struct stat *st) {
    if(strstr(p,"b.bin") && stat_failures) {
        if(stat_failures>0) { --stat_failures; }
        errno=EIO; return -1;
    }
    return stat(p,st);
}
static int fault_rename(const char *a,const char *b) {
    if(publish_failures && strstr(a,".tmp")) { --publish_failures; errno=EIO; return -1; }
    return rename(a,b);
}
static const char *rg_basename(const char *p) { const char *s=strrchr(p,'/'); return s?s+1:p; }
static const char *rg_extension(const char *p) { const char *s=strrchr(p,'.'); return s?s+1:""; }
#define fopen fault_open
#define fread fault_read
#define fwrite fault_write
#define fclose fault_close
#define readdir fault_readdir
#define rename fault_rename
#define stat fault_stat
'''
    # A macro named stat must not change `struct stat` in production declarations.
    source = source.replace('#define stat fault_stat', '#define stat(p,s) fault_stat(p,s)')
    source += section(STORAGE, 'rg_stat_t rg_storage_stat(', 'int64_t rg_storage_get_free_space(')
    source += section(STORAGE, 'FILE *rg_storage_open_file(', '/**\n * This is a minimal UNZIP')
    source += r'''
#define FLASH_CHUNK_SIZE 4096
#define MEM_FAST 0
#define MEM_NOPANIC 0
#define C_BLACK 0
static void *rg_alloc(size_t n,int caps) { (void)caps; return malloc(n); }
static void rg_display_clear(int c) { (void)c; }
static void rg_system_tick(int n) { (void)n; }
static void draw_firmware_message(const char *s,...) { (void)s; }
static uint32_t rg_crc32(uint32_t crc,const uint8_t *p,size_t n) {
    crc=~crc; while(n--) { crc^=*p++; for(int i=0;i<8;i++) crc=(crc>>1)^((crc&1)?0xedb88320:0); } return ~crc;
}
'''
    source += section(FIRMWARE, 'static bool read_exact(', 'static rg_rect_t draw_firmware_message(')
    source += section(FIRMWARE, 'static const char *verify_image_crc(', 'static int read_partition_table(')
    source += r'''
#define ESP_OK 0
#define FLASH_SECTOR_SIZE 4096
typedef int esp_err_t;
static void *esp_flash_default_chip;
static uint8_t flash_bytes[32768];
static int erases,flash_reads_fail;
static int esp_flash_read(void *chip,void *out,uint32_t offset,size_t n) {
    (void)chip; if(flash_reads_fail)return -1; assert(offset+n<=sizeof(flash_bytes)); memcpy(out,flash_bytes+offset,n); return 0;
}
static int esp_flash_write(void *chip,const void *data,uint32_t offset,size_t n) {
    (void)chip; assert(offset+n<=sizeof(flash_bytes)); memcpy(flash_bytes+offset,data,n); return 0;
}
static int esp_flash_erase_region(void *chip,uint32_t offset,size_t n) {
    (void)chip; ++erases; assert(offset+n<=sizeof(flash_bytes)); memset(flash_bytes+offset,255,n); return 0;
}
'''
    source += section(FIRMWARE, 'static int flash_range_matches(', '/**')
    source += section(FIRMWARE, 'static const char *write_flash_range(', 'static bool range_overlaps(')
    source += r'''
static int seen_a,seen_b,scan_error;
static int collect(const rg_scandir_t *e,void *arg) {
    (void)arg;
    if(scan_error) return RG_SCANDIR_ERROR;
    if(!strcmp(e->basename,"a.bin")) ++seen_a;
    if(!strcmp(e->basename,"b.bin")) ++seen_b;
    return RG_SCANDIR_CONTINUE;
}
static void create(const char *p,const void *data,size_t n) {
    FILE *fp=fopen(p,"wb"); assert(fp); assert(fwrite(data,1,n,fp)==n); assert(fclose(fp)==0);
}
int main(int argc,char **argv) {
    assert(argc==2); char file[256],a[256],b[256],sub[256];
    snprintf(file,sizeof(file),"%s/image.bin",argv[1]);
    uint8_t data[32768],out[32768]; for(size_t i=0;i<sizeof(data);i++) data[i]=(uint8_t)(i*7);
    create(file,data,sizeof(data)); FILE *fp=fopen(file,"rb");
    int before=opens; read_failures=1;
    assert(rg_storage_read_at(&fp,file,8192,out,8192));
    assert(opens>before && !memcmp(out,data+8192,8192)); fclose(fp);
    fp=fopen(file,"rb"); read_failures=-1;
    assert(!rg_storage_read_at(&fp,file,0,out,512)); assert(!fp); read_failures=0;
    assert(rg_storage_read_at(&fp,file,0,out,512)); assert(!memcmp(out,data,512)); fclose(fp);
    void *whole=NULL; size_t len=0; read_failures=1;
    assert(rg_storage_read_file(file,&whole,&len,0)); assert(len==sizeof(data) && !memcmp(whole,data,len)); free(whole);
    fp=fopen(file,"rb"); bad_reads=1; before=opens;
    assert(!verify_image_crc(&fp,file,sizeof(data),rg_crc32(0,data,sizeof(data)),false,NULL));
    assert(opens>before); fclose(fp);
    fp=fopen(file,"rb"); bad_reads=-1;
    assert(verify_image_crc(&fp,file,sizeof(data),rg_crc32(0,data,sizeof(data)),false,NULL));
    bad_reads=0; if(fp)fclose(fp);
    memset(flash_bytes,255,sizeof(flash_bytes));
    fp=fopen(file,"rb"); read_failures=-1;
    assert(write_flash_range(&fp,file,0,0,4096,"app") && !erases); read_failures=0; if(fp)fclose(fp);
    fp=fopen(file,"rb"); flash_reads_fail=1;
    assert(write_flash_range(&fp,file,0,0,4096,"app") && !erases); flash_reads_fail=0; fclose(fp);
    fp=fopen(file,"rb"); assert(!write_flash_range(&fp,file,0,0,4096,"app"));
    assert(erases==1 && !memcmp(flash_bytes,data,4096)); fclose(fp);
    write_failures=1; close_failures=1;
    assert(rg_storage_write_file(file,data,sizeof(data),RG_FILE_ATOMIC_WRITE));
    before=opens; close_failures=1;
    assert(rg_storage_write_file(file,data,sizeof(data),RG_FILE_ATOMIC_WRITE)); assert(opens>=before+2);
    publish_failures=1;
    assert(!rg_storage_write_file(file,"replacement",11,RG_FILE_ATOMIC_WRITE));
    write_failures=-1;
    assert(!rg_storage_write_file(file,"bad",3,RG_FILE_ATOMIC_WRITE)); write_failures=0;
    fp=fopen(file,"rb"); assert(fread(out,1,sizeof(out),fp)==sizeof(out)); assert(!memcmp(out,data,sizeof(out))); fclose(fp);
    snprintf(sub,sizeof(sub),"%s/list",argv[1]); assert(!mkdir(sub,0700));
    snprintf(a,sizeof(a),"%s/list/a.bin",argv[1]); snprintf(b,sizeof(b),"%s/list/b.bin",argv[1]);
    create(a,"a",1); create(b,"b",1);
    directory_failures=1; unknown_type=1; stat_failures=1;
    assert(rg_storage_scandir(sub,collect,NULL,0)); assert(seen_a==1 && seen_b==1);
    seen_a=seen_b=0; directory_failures=-1;
    assert(!rg_storage_scandir(sub,collect,NULL,0)); assert(seen_a<=1); directory_failures=0;
    stat_failures=-1; assert(!rg_storage_scandir(sub,collect,NULL,RG_SCANDIR_STAT)); stat_failures=0;
    scan_error=1; assert(!rg_storage_scandir(sub,collect,NULL,0)); scan_error=0;
    char child[256],nested[256];
    snprintf(child,sizeof(child),"%s/list/child",argv[1]); assert(!mkdir(child,0700));
    snprintf(nested,sizeof(nested),"%s/list/child/nested-failure.bin",argv[1]); create(nested,"x",1);
    nested_failures=1; assert(!rg_storage_scandir(sub,collect,NULL,RG_SCANDIR_RECURSIVE));
    puts("File recovery, CRC retries, atomic write failure, directory resumption/error detection passed");
}
'''
    run_c(cc, folder, 'files', source, folder)


def test_spi(cc, folder):
    defines = '\n'.join(line for line in STORAGE.splitlines() if line.startswith(('#define SDMMC_CMD_', '#define SDCARD_MAX_', '#define SDCARD_RECOVERY_')))
    source = BASE + defines + r'''
#define CONFIG_IDF_TARGET_ESP32S3 1
#define ESP_IDF_VERSION_MAJOR 5
#define RG_STORAGE_SDSPI_HOST 1
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_ERR_INVALID_CRC 3
#define ESP_ERR_NOT_FOUND 4
#define SCF_CMD_AC 0
#define SCF_RSP_R1B 0
#define SCF_RSP_R1 0
#define SD_OCR_SDHC_CAP 0x40000000
#define RG_INDICATOR_ACTIVITY_DISK 0
#define RG_INDICATOR_ACTIVITY_DISK_READ 1
#define RG_INDICATOR_ACTIVITY_DISK_WRITE 2
typedef int esp_err_t;
typedef int rg_indicator_t;
typedef struct { uint32_t opcode,arg,response[4],timeout_ms; int flags,error; void *data; size_t datalen,blklen,buflen; } sdmmc_command_t;
typedef struct { unsigned rca,ocr; struct { int max_freq_khz; int (*set_card_clk)(int,uint32_t); } host; } sdmmc_card_t;
static sdmmc_card_t card,*card_handle=&card;
static bool sdcard_single_block_reads, sdcard_ocr_valid;
static uint32_t sdcard_ocr;
static int failures,stop_calls,status_calls,data_calls,other_calls,clocks;
static uint32_t args[128],ops[128];
static int set_clock(int slot,uint32_t hz) { (void)slot; assert(hz>=1000); ++clocks; return ESP_OK; }
static int64_t rg_system_timer(void) { static int64_t t; return t+=1000; }
static void rg_system_set_indicator(int a,int b) { (void)a;(void)b; }
static int raw_transaction(int slot,sdmmc_command_t *cmd) {
    (void)slot;
    if(cmd->opcode==58) { cmd->response[0]=card.ocr; return ESP_OK; }
    if(cmd->opcode==12) { ++stop_calls; return ESP_OK; }
    if(cmd->opcode==13) { ++status_calls; return ESP_OK; }
    if(cmd->opcode!=17 && cmd->opcode!=18 && cmd->opcode!=24 && cmd->opcode!=25) { ++other_calls; return ESP_ERR_INVALID_CRC; }
    assert(data_calls<128); args[data_calls]=cmd->arg; ops[data_calls++]=cmd->opcode;
    if(failures) {
        if(failures>0)--failures;
        cmd->error=ESP_ERR_INVALID_CRC; cmd->arg+=999; return ESP_ERR_INVALID_CRC;
    }
    assert(!cmd->error);
    if(cmd->opcode==17 || cmd->opcode==18) memset(cmd->data,cmd->arg&255,cmd->datalen);
    return ESP_OK;
}
#define SDCARD_DO_TRANSACTION raw_transaction
#if 1
'''
    source += section(STORAGE, 'static bool sdcard_is_data_cmd(', 'void rg_storage_init(')
    source += r'''
static void reset(void) {
    memset(&card,0,sizeof(card)); card.ocr=SD_OCR_SDHC_CAP; card.host.max_freq_khz=10000; card.host.set_card_clk=set_clock;
    failures=stop_calls=status_calls=data_calls=other_calls=clocks=0; sdcard_single_block_reads=false; sdcard_ocr_valid=false; card_handle=&card;
}
int main(void) {
    uint8_t data[1536]; reset();
    sdmmc_command_t cmd={.opcode=25,.arg=100,.data=data,.datalen=1536,.blklen=512,.buflen=1536};
    failures=1; assert(sdcard_do_transaction(1,&cmd)==ESP_OK);
    assert(data_calls==4 && ops[0]==24 && ops[1]==24 && args[0]==100 && args[1]==100 && args[2]==101 && args[3]==102);
    assert(!stop_calls && clocks==1 && card.host.max_freq_khz==5000);
    reset(); cmd.opcode=18; cmd.arg=200; failures=1;
    assert(sdcard_do_transaction(1,&cmd)==ESP_OK); assert(stop_calls==1 && data_calls==2 && args[0]==200 && args[1]==200);
    data_calls=0; assert(sdcard_do_transaction(1,&cmd)==ESP_OK);
    assert(data_calls==3 && ops[0]==17 && args[0]==200 && args[1]==201 && args[2]==202);
    assert(data[0]==200 && data[512]==201 && data[1024]==202);
    reset(); card.ocr=0; cmd.opcode=25; cmd.arg=51200;
    assert(sdcard_do_transaction(1,&cmd)==ESP_OK); assert(args[0]==51200 && args[1]==51712 && args[2]==52224);
    reset(); cmd.opcode=41; assert(sdcard_do_transaction(1,&cmd)==ESP_ERR_INVALID_CRC); assert(other_calls==1 && !clocks);
    reset(); cmd.opcode=18; failures=-1;
    assert(sdcard_do_transaction(1,&cmd)==ESP_ERR_INVALID_CRC);
    assert(data_calls==SDCARD_MAX_RETRIES+1 && stop_calls==SDCARD_MAX_RETRIES+1);
    assert(card.host.max_freq_khz==1000);
    failures=0; data_calls=0; cmd.arg=100; cmd.error=0;
    assert(sdcard_do_transaction(1,&cmd)==ESP_OK); assert(data_calls==3 && ops[0]==17);
    reset(); card_handle=NULL; sdmmc_command_t ocr_cmd={.opcode=58};
    assert(sdcard_do_transaction(1,&ocr_cmd)==ESP_OK);
    cmd.opcode=25; cmd.arg=100; cmd.error=0;
    assert(sdcard_do_transaction(1,&cmd)==ESP_OK); assert(data_calls==3 && args[1]==101);
    reset(); card.ocr=0; card_handle=NULL; ocr_cmd.error=0;
    assert(sdcard_do_transaction(1,&ocr_cmd)==ESP_OK);
    cmd.opcode=25; cmd.arg=51200; cmd.error=0;
    assert(sdcard_do_transaction(1,&cmd)==ESP_OK); assert(args[1]==51712);
    puts("SPI command retries, lower clock, sector addressing, write isolation, failed-stream cleanup passed");
}
'''
    run_c(cc, folder, 'spi', source)


def test_snapshot(cc, folder):
    source = BASE + r'''
static int fail_scan,cache_writes,oom;
typedef struct retro_app_t retro_app_t;
typedef struct { int marker; retro_app_t *app; } retro_file_t;
struct retro_app_t { char *short_name; struct { char *roms; } paths; size_t files_count,files_capacity; retro_file_t *files; void *filenames; bool cached; int cached_count; };
#define RG_SCANDIR_RECURSIVE 1
static void *rg_bucket_create(int n) { (void)n; return oom?NULL:malloc(32); }
static void rg_bucket_free(void *p) { free(p); }
static int scan_folder_cb(void *e,void *a) { (void)e;(void)a; return 1; }
static bool rg_storage_scandir(const char *p,int (*cb)(void*,void*),void *arg,int flags) {
    (void)p;(void)cb;(void)flags; retro_app_t *app=arg;
    app->files[0].marker=11; app->files[1].marker=22; app->files_count=2; return !fail_scan;
}
static void rg_gui_draw_message(const char *s,...) { (void)s; }
static void rg_gui_alert(const char *s,const char *m) { (void)s;(void)m; }
static bool romcache_save(retro_app_t *app) { assert(app->files_count==2); ++cache_writes; return true; }
'''
    # The delay stub in BASE is unused in this small isolated snapshot test.
    source = source.replace('static void rg_task_delay(int ms) { (void)ms; }', '')
    source += section(APPS, 'static bool application_scan(', 'static void application_init(')
    source += r'''
int main(void) {
    retro_app_t app={.short_name="nes",.files_count=1,.files_capacity=1,.cached=true,.cached_count=1};
    app.paths.roms="roms/nes"; app.files=calloc(1,sizeof(retro_file_t)); app.files[0].marker=99; app.filenames=malloc(32);
    retro_file_t *old=app.files; void *names=app.filenames;
    fail_scan=1; assert(!application_scan(&app,false));
    assert(app.files==old && app.filenames==names && app.files_count==1 && app.files[0].marker==99 && cache_writes==0);
    oom=1; assert(!application_scan(&app,false)); assert(app.files==old && !cache_writes); oom=0;
    fail_scan=0; assert(application_scan(&app,false));
    assert(app.files_count==2 && cache_writes==1 && app.files[0].app==&app && app.files[1].app==&app);
    free(app.files); free(app.filenames); puts("Incomplete ROM scans preserve the previous list and cache; completed snapshots publish correctly");
}
'''
    run_c(cc, folder, 'snapshot', source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        test_files(args.cc, folder)
        test_spi(args.cc, folder)
        test_snapshot(args.cc, folder)


if __name__ == '__main__':
    main()
