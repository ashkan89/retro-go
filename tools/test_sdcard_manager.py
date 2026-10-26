#!/usr/bin/env python3
"""Exercise the firmware's actual ZIP installer with host filesystem adapters."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import zipfile
import release

ROOT = Path(__file__).resolve().parents[1]


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
    print("SD installer: streaming, directories, empty/stored/deflated files, unsafe paths, CRC and truncated ZIPs passed; SD format failure paths passed")


if __name__ == "__main__":
    main()
