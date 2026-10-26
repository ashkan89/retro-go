#!/usr/bin/env python3
"""Exercise the firmware's actual ZIP installer with host filesystem adapters."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import zipfile
import release
import json

ROOT = Path(__file__).resolve().parents[1]


def test_release_lookup(cc, folder, source):
    resolver = source[source.index("typedef enum {\n    ARCHIVE_FOUND"):source.index("static bool download_archive(")]
    harness = folder / "release_lookup.c"
    harness.write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "cJSON.h"
#include "update_version.h"
#define RG_UPDATER_GITHUB_RELEASES "https://api.github.com/repos/ashkan89/retro-go/releases/latest"
#define ARCHIVE_LIMIT INT_MAX
#define RG_MIN(a,b) ((a)<(b)?(a):(b))
typedef struct { const char *name, *value; } rg_http_header_t;
typedef struct { bool verify_server; rg_http_header_t *headers; int timeout_ms; } rg_http_cfg_t;
typedef struct { int status_code, content_length; } rg_http_req_t;
typedef struct { const char *version; } rg_app_t;
#define RG_HTTP_DEFAULT_CONFIG() ((rg_http_cfg_t){0})
static char *body;
static size_t position;
static rg_app_t app;
static rg_http_req_t request;
static rg_app_t *rg_system_get_app(void) { return &app; }
static void rg_system_tick(int n) { (void)n; }
static rg_http_req_t *rg_network_http_open(const char *url, rg_http_cfg_t *cfg) {
    if (!strstr(url, "/releases/tags/v5.0.1") || !cfg->verify_server) abort();
    return &request;
}
static int rg_network_http_read(rg_http_req_t *req, void *out, size_t size) {
    size_t count = RG_MIN(size, (size_t)req->content_length - position);
    memcpy(out, body + position, count); position += count; return (int)count;
}
static void rg_network_http_close(rg_http_req_t *req) { (void)req; }
''' + resolver + r'''
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    FILE *fp = fopen(argv[1], "rb"); if (!fp) return 3;
    fseek(fp, 0, SEEK_END); long size = ftell(fp); rewind(fp);
    body = calloc(1, (size_t)size + 1); if (!body) return 4;
    if (fread(body, 1, size, fp) != (size_t)size) return 5;
    fclose(fp); request.content_length = (int)size; request.status_code = atoi(argv[3]);
    app.version = argv[2]; char url[512] = {0}; int bytes = 0;
    int result = resolve_archive("sdcard_data.zip", url, sizeof(url), &bytes);
    printf("%d %d", result, bytes); free(body); return 0;
}
''')
    executable = folder / "release_lookup"
    cjson = ROOT / "components/retro-go/libs/cJSON"
    subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-I", str(cjson), "-I", str(ROOT / "launcher/main"), str(harness),
                    str(cjson / "cJSON.c"), "-lm", "-o", str(executable)], check=True)
    fixture = folder / "release.json"
    def check(size, expected, name="sdcard_data.zip", version="v5.0.1", status=200, tag="v5.0.1"):
        fixture.write_text(json.dumps({"tag_name": tag, "draft": False, "assets": [
            {"name": name, "size": size, "browser_download_url":
             "https://github.com/ashkan89/retro-go/releases/download/v5.0.1/" + name}]}))
        output = subprocess.check_output([str(executable), str(fixture), version, str(status)], text=True).split()
        assert int(output[0]) == expected, output
        if expected == 0: assert int(output[1]) == size
    check(163528209, 0)  # Actual v5.0.1 data asset, larger than the old 64 MB limit.
    check(2147483647, 0)
    check(2147483648, 3)
    check(80323, 2, name="sdcard_structure.zip")
    check(163528209, 4, version="v5.0.1-dirty")
    check(163528209, 1, status=404)
    check(163528209, 1, tag="v5.0.0")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = (ROOT / "launcher/main/sdcard_manager.c").read_text()
    installer = source[source.index("static bool archive_path("):source.index("static void sdcard_manager_run(")]
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        harness = folder / "installer.c"
        harness.write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include "miniz.h"
#define RG_PATH_MAX 255
#define _(x) (x)
static char *card_root, *archive_file;
static char install_temp[RG_PATH_MAX];
#define INSTALL_TEMP_PATH install_temp
#define RG_STORAGE_ROOT card_root
#define ARCHIVE_PATH archive_file
static uint64_t total_bytes, installed_bytes;
static int64_t last_draw;
static const char *stage;
static int64_t rg_system_timer(void) { return 0; }
static void rg_system_tick(int n) { (void)n; }
static void progress(const char *s, int n) { (void)s; (void)n; }
static int64_t rg_storage_get_free_space(const char *p) { (void)p; return 1024LL*1024*1024; }
static bool rg_storage_delete(const char *p) { return remove(p) == 0; }
static bool rg_storage_mkdir(const char *p) {
    char copy[RG_PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", p);
    for (char *c = copy + 1; *c; ++c) {
        if (*c == '/') { *c = 0; if (mkdir(copy, 0700) && errno != EEXIST) return false; *c = '/'; }
    }
    return mkdir(copy, 0700) == 0 || errno == EEXIST;
}
''' + installer + r'''
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    archive_file = argv[1]; card_root = argv[2];
    snprintf(install_temp, sizeof(install_temp), "%s/.sdcard_install.tmp", card_root);
    return install_archive() ? 0 : 1;
}
''')
        executable = folder / "installer"
        miniz = ROOT / "components/retro-go/libs/miniz"
        subprocess.run([args.cc, "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-Dtinfl_decompress=sdcard_tinfl_decompress", "-DMINIZ_NO_DEFLATE_APIS", "-DMINIZ_NO_ZLIB_APIS",
                        "-I", str(miniz), str(harness), str(miniz / "miniz.c"), "-o", str(executable)], check=True)

        def run_case(name, entries, expected, corrupt=False, truncate=False, existing=None):
            card = folder / name
            card.mkdir()
            for filename, content in (existing or {}).items():
                dest = card / filename
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(content)
            archive = card / ".sdcard_structure.zip"
            with zipfile.ZipFile(archive, "w") as output:
                for filename, content, method in entries:
                    output.writestr(filename, content, compress_type=method)
            if corrupt:
                data = bytearray(archive.read_bytes())
                pos = data.index(b"CRC_TEST_PAYLOAD")
                data[pos] ^= 1
                archive.write_bytes(data)
            if truncate:
                archive.write_bytes(archive.read_bytes()[:-12])
            result = subprocess.run([str(executable), str(archive), str(card)])
            assert result.returncode == (0 if expected else 1), name
            return card

        stored, compressed = zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED
        payload = bytes(range(256)) * 4096  # Verify bounded streaming across many output chunks.
        card = run_case("valid", [("roms/nes/", b"", stored), ("retro-go/bios/msx/", b"", compressed),
                                 ("nested/large.bin", payload, compressed), ("empty.txt", b"", stored),
                                 ("stored.txt", b"hello", stored)], True)
        assert (card / "nested/large.bin").read_bytes() == payload
        assert (card / "empty.txt").read_bytes() == b""
        card = run_case("overwrite", [("roms/nes/game.nes", b"replacement", compressed),
                                      ("new/nested/file.txt", b"new file", stored)], True,
                        existing={"roms/nes/game.nes": b"old longer game content", "keep/save.sav": b"keep"})
        assert (card / "roms/nes/game.nes").read_bytes() == b"replacement"
        assert (card / "new/nested/file.txt").read_bytes() == b"new file"
        assert (card / "keep/save.sav").read_bytes() == b"keep"
        assert not (card / ".sdcard_install.tmp").exists()
        card = run_case("overwrite-crc", [("game.bin", b"CRC_TEST_PAYLOAD", stored)], False,
                        corrupt=True, existing={"game.bin": b"original game"})
        assert (card / "game.bin").read_bytes() == b"original game"
        assert not (card / ".sdcard_install.tmp").exists()
        card = folder / "release-structure"
        card.mkdir()
        archive = card / ".sdcard_structure.zip"
        release.create_sdcard_structure(archive, "4.9.6")
        subprocess.run([str(executable), str(archive), str(card)], check=True)
        assert (card / "retro-go/themes/default/theme.json").read_bytes() == (ROOT / "themes/default/theme.json").read_bytes()
        assert (card / "retro-go/bios/msx").is_dir()
        for i, unsafe in enumerate(("../escape", "/absolute", "nested/../../escape", "a\\escape", "C:/escape",
                                    "./file", "a//file", "x" * 300, ".sdcard_structure.zip", ".SDCARD_STRUCTURE.ZIP",
                                    "trailing./file", "trailing /file", "bad?/file", "control\nfile", ".sdcard_install.tmp", ".SDCARD_INSTALL.TMP")):
            card = run_case(f"unsafe{i}", [("before.txt", b"must not be installed", stored),
                                            (unsafe, b"bad", stored)], False)
            assert not (card / "before.txt").exists(), "Preflight must reject before writing any entries"
        card = run_case("crc", [("bad.txt", b"CRC_TEST_PAYLOAD", stored)], False, corrupt=True)
        assert not (card / "bad.txt").exists(), "Remove corrupt output"
        run_case("truncated", [("file", b"hello", compressed)], False, truncate=True)
        run_case("empty", [], False)
        storage_source = (ROOT / "components/retro-go/rg_storage.c").read_text()
        formatter = storage_source[storage_source.index("static bool sdcard_has_requested_format("):storage_source.index("bool rg_storage_ready(")]
        formatter = formatter.replace("\n#endif\n\nbool rg_storage_format", "\n\nbool rg_storage_format", 1)
        harness.write_text(r'''
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define CONFIG_IDF_TARGET_ESP32S3 1
#define RG_STORAGE_SDSPI_HOST 1
#define RG_STORAGE_ROOT "/sd"
#define RG_STORAGE_FORMAT_CLUSTER_SIZE 512
#define RG_LOGE(...) ((void)0)
#define FR_OK 0
#define FS_FAT32 3
#define FM_FAT32 2
typedef unsigned char BYTE;
typedef int FRESULT;
typedef struct { int fs_type, csize; } FATFS;
typedef struct { struct { FATFS *fs; } obj; } FF_DIR;
typedef struct { int fmt, n_fat, au_size; } MKFS_PARM;
typedef struct { struct { int sector_size; } csd; } sdmmc_card_t;
static sdmmc_card_t card;
static FATFS filesystem;
static bool disk_mounted, format_mount_requested, init_ok, mountable, registered, remount_ok, correct_format;
static sdmmc_card_t *card_handle;
static int format_result, init_calls, format_calls, deinit_calls;
static void rg_storage_init(void) {
    ++init_calls;
    if (init_ok && (mountable || format_mount_requested)) { disk_mounted = true; card_handle = &card; }
}
static void rg_storage_deinit(void) { ++deinit_calls; card_handle = 0; disk_mounted = false; }
static int ff_diskio_get_pdrv_card(sdmmc_card_t *p) { (void)p; return registered ? 0 : 255; }
static int f_opendir(FF_DIR *dir, const char *path) {
    (void)path; dir->obj.fs = &filesystem;
    filesystem.fs_type = correct_format ? FS_FAT32 : 1; filesystem.csize = 1;
    return remount_ok ? FR_OK : -1;
}
static int f_closedir(FF_DIR *dir) { (void)dir; return FR_OK; }
static FRESULT f_mkfs(const char *drive, const MKFS_PARM *cfg, void *work, unsigned size) {
    assert(drive[0] == '0' && work && size == 32768);
    assert(cfg->fmt == FM_FAT32 && cfg->au_size == 512 && cfg->n_fat == 2);
    ++format_calls; return format_result;
}
''' + formatter + r'''
static void reset(void) {
    card.csd.sector_size = 512;
    disk_mounted = true; card_handle = &card; format_mount_requested = false;
    init_ok = mountable = registered = remount_ok = correct_format = true; format_result = 0;
    init_calls = format_calls = deinit_calls = 0;
}
int main(void) {
    reset(); assert(rg_storage_format()); assert(format_calls == 1 && init_calls == 0);
    reset(); card_handle = 0; assert(!rg_storage_format()); assert(format_calls == 0);
    reset(); disk_mounted = false; card_handle = 0;
    assert(rg_storage_format()); assert(init_calls == 1 && format_calls == 1 && !format_mount_requested);
    reset(); disk_mounted = false; card_handle = 0; mountable = false;
    assert(rg_storage_format()); assert(init_calls == 2 && format_calls == 0 && !format_mount_requested);
    reset(); disk_mounted = false; card_handle = 0; init_ok = false;
    assert(!rg_storage_format()); assert(!format_mount_requested && format_calls == 0);
    reset(); registered = false; assert(!rg_storage_format()); assert(format_calls == 0);
    reset(); format_result = -1; assert(!rg_storage_format()); assert(deinit_calls == 1 && !disk_mounted);
    reset(); remount_ok = false; assert(!rg_storage_format()); assert(deinit_calls == 1);
    reset(); correct_format = false; assert(!rg_storage_format()); assert(deinit_calls == 1);
    reset(); card.csd.sector_size = 4096; assert(!rg_storage_format()); assert(format_calls == 0);
    return 0;
}
''')
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        test_release_lookup(args.cc, folder, source)
    print("SD installer: streaming, directories, empty/stored/deflated files, unsafe paths, CRC and truncated ZIPs passed; SD format failure paths and large release asset lookup passed")


if __name__ == "__main__":
    main()
