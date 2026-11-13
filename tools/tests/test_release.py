import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib
import zipfile
import hashlib

spec = importlib.util.spec_from_file_location("release", Path(__file__).parents[1] / "release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
image_spec = importlib.util.spec_from_file_location("mkfw", Path(__file__).parents[1] / "mkfw.py")
mkfw = importlib.util.module_from_spec(image_spec)
image_spec.loader.exec_module(mkfw)


def image_bytes(version, target, apps=None, chip_type="esp32s3", partition_size=0x10000):
    if apps is None:
        apps = release.APPS + (("gbsp",) if target.startswith((
            "esp32-p4-n16r32-", "esp32-s3-n16r8-")) else ())
    payload = bytearray(256)
    payload[0] = 0xE9
    struct.pack_into("<I", payload, 32, 0xABCD5432)
    payload[48:48 + len(version)] = version.encode()
    partitions = [(0, 0 if label == "factory" else 16, label,
                   0x10000 + partition_size * i, partition_size, payload) for i, label in enumerate(apps)]
    with tempfile.TemporaryDirectory() as folder:
        bootloader = Path(folder) / "bootloader.bin"
        bootloader.write_bytes(b"\xe9\x00")
        return mkfw.create_image(chip_type, partitions, str(bootloader), "Retro-Go", version, target)


class ReleaseTests(unittest.TestCase):
    def test_raw_app_partition_sizes_use_sectors_and_aligned_starts(self):
        args = ["0", "0", "0x11001", "factory", "factory.bin",
                "0", "16", "0x1BF000", "retro-legacy", "legacy.bin"]
        with patch.object(mkfw, "readfile", return_value=b"\xe9"):
            parts = mkfw.get_partitions(args.copy(), 0x10000, app_size_alignment=0x1000)
            self.assertEqual([(p[3], p[4]) for p in parts],
                             [(0x10000, 0x12000), (0x30000, 0x1BF000)])
            # Keep the existing .fw layout for devices which use that format.
            fw_parts = mkfw.get_partitions(args.copy())
            self.assertEqual([(p[3], p[4]) for p in fw_parts],
                             [(0, 0x20000), (0x20000, 0x1C0000)])

    def test_s3_partition_capacity(self):
        for panel in ("ili9341", "st7796", "st7789", "st7789v2"):
            for memory, capacity in (("n16r8", 16 << 20), ("n8r2", 8 << 20)):
                target = f"esp32-s3-{memory}-{panel}"
                namespace = {"PROJECT_APPS": {app: [0, 0, 1 << 20] for app in release.APPS + ("gbsp",)}}
                env_file = release.ROOT / "components/retro-go/targets" / target / "env.py"
                exec(compile(env_file.read_text(), str(env_file), "exec"), namespace)
                apps = namespace["DEFAULT_APPS"].split()
                size = 0x10000 + sum(namespace["PROJECT_APPS"][app][2] for app in apps) + 256
                with self.subTest(target=target):
                    self.assertLessEqual(size, capacity)
                    self.assertEqual("gbsp" in apps, memory == "n16r8")
                    if memory == "n8r2":
                        self.assertGreater(size + 0x200000, capacity)

    def test_s3_gba_memory_variants(self):
        for panel in ("ili9341", "st7796", "st7789", "st7789v2"):
            for memory in ("n16r8", "n8r2"):
                target = f"esp32-s3-{memory}-{panel}"
                with self.subTest(target=target), tempfile.TemporaryDirectory() as folder:
                    path = Path(folder) / "s3.img"
                    path.write_bytes(image_bytes("5.2.1", target))
                    release.validate_image(path, "5.2.1", target)
                    wrong_apps = release.APPS if memory == "n16r8" else release.APPS + ("gbsp",)
                    path.write_bytes(image_bytes("5.2.1", target, wrong_apps))
                    with self.assertRaisesRegex(ValueError, "missing or unexpected applications"):
                        release.validate_image(path, "5.2.1", target)

    def test_p4_bootloader_and_16mb_image(self):
        target = "esp32-p4-n16r32-st7796"
        data = image_bytes("5.2.1", target, chip_type="esp32p4", partition_size=0x140000)
        self.assertGreater(len(data), 8 * 1024 * 1024)
        self.assertEqual(data[:0x2000], b"\xff" * 0x2000)
        self.assertEqual(data[0x2000:0x2002], b"\xe9\x00")
        self.assertEqual(data[0x8000:0x8002], b"\xaa\x50")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "p4.img"
            path.write_bytes(data)
            result = release.validate_image(path, "5.2.1", target)
            self.assertEqual(result["target"], target)

    def test_release_changelog(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "CHANGELOG.md").write_text(
                "# Retro-Go 4.5\n\n## Updates\n\n- New updater.\n\n# Retro-Go 4.4\n\n- Old change.\n")
            with patch.object(release, "ROOT", Path(folder)):
                notes = release.release_notes("v4.5")
                self.assertIn("## Updates", notes)
                self.assertIn("New updater.", notes)
                self.assertNotIn("Old change.", notes)

    def test_p4_ili9341_requires_gba(self):
        target = "esp32-p4-n16r32-ili9341"
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "p4-ili9341.img"
            path.write_bytes(image_bytes("5.2.1", target, chip_type="esp32p4"))
            self.assertEqual(release.validate_image(path, "5.2.1", target)["target"], target)
            path.write_bytes(image_bytes("5.2.1", target, release.APPS, chip_type="esp32p4"))
            with self.assertRaisesRegex(ValueError, "missing or unexpected applications"):
                release.validate_image(path, "5.2.1", target)

    def test_tags(self):
        for tag in ("v4.5", "v4.5.1", "v0.0.0"):
            self.assertEqual(release.release_version(tag), tag[1:])
        for tag in ("4.5", "v4", "v4.5-rc1", "v4.5/evil", "v04.5", "v42949672960.0"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.release_version(tag)

    def test_image_validation(self):
        target = release.release_targets()[0]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / f"retro-go_4.5_{target}.img"
            path.write_bytes(image_bytes("4.5", target))
            self.assertEqual(release.validate_image(path, "4.5", target)["target"], target)
            with self.assertRaises(ValueError): release.validate_image(path, "4.6", target)
            with self.assertRaises(ValueError): release.validate_image(path, "4.5", "wrong-target")
            corrupted = bytearray(path.read_bytes())
            corrupted[0x10001] ^= 1
            path.write_bytes(corrupted)
            with self.assertRaises(ValueError): release.validate_image(path, "4.5", target)
            path.write_bytes(image_bytes("4.5", target, release.APPS[1:]))
            with self.assertRaises(ValueError): release.validate_image(path, "4.5", target)
            wrong_app_version = bytearray(image_bytes("4.5", target))
            wrong_app_version[0x10000 + 48:0x10000 + 51] = b"4.4"
            struct.pack_into("<I", wrong_app_version, len(wrong_app_version) - 256 + 96,
                             zlib.crc32(wrong_app_version[:-256]))
            path.write_bytes(wrong_app_version)
            with self.assertRaisesRegex(ValueError, "built with a different version"):
                release.validate_image(path, "4.5", target)

    def test_complete_package(self):
        with tempfile.TemporaryDirectory() as folder:
            for target in release.release_targets():
                (Path(folder) / f"retro-go_4.5_{target}.img").write_bytes(image_bytes("4.5", target))
            release.package_release(folder, "v4.5")
            manifest = json.loads((Path(folder) / "release-manifest.json").read_text())
            expected_assets = len(release.release_targets()) + 1
            self.assertEqual(len(manifest["assets"]), expected_assets)
            self.assertEqual(len((Path(folder) / "SHA256SUMS").read_text().splitlines()), expected_assets)
            structure = Path(folder) / "sdcard_structure.zip"
            with zipfile.ZipFile(structure) as archive:
                self.assertIsNone(archive.testzip())
                self.assertIn("roms/nes/", archive.namelist())
                self.assertIn("retro-go/bios/msx/", archive.namelist())
                self.assertIn("retro-go/firmware/", archive.namelist())
                self.assertIn("retro-go/themes/default/theme.json", archive.namelist())
                for theme in (release.ROOT / "themes").iterdir():
                    if theme.is_dir() and (theme / "theme.json").is_file():
                        for name in ("theme.json", "preview.png", "logo_gba.png", "banner_gba.png", "background_gba.png"):
                            self.assertEqual(archive.read(f"retro-go/themes/{theme.name}/{name}"),
                                             (theme / name).read_bytes())
                self.assertIn("4.5", archive.read("SDCARD_README.txt").decode())
                for name in archive.namelist():
                    self.assertFalse(name.startswith("/"))
                    self.assertNotIn("..", Path(name).parts)
            asset = manifest["assets"][-1]
            self.assertEqual(asset["sha256"], hashlib.sha256(structure.read_bytes()).hexdigest())
            next(Path(folder).glob("*.img")).unlink()
            with self.assertRaises(ValueError): release.package_release(folder, "v4.5")


if __name__ == "__main__":
    unittest.main()
