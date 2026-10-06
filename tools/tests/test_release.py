import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

spec = importlib.util.spec_from_file_location("release", Path(__file__).parents[1] / "release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
image_spec = importlib.util.spec_from_file_location("mkfw", Path(__file__).parents[1] / "mkfw.py")
mkfw = importlib.util.module_from_spec(image_spec)
image_spec.loader.exec_module(mkfw)


def image_bytes(version, target, apps=release.APPS):
    payload = bytearray(256)
    payload[0] = 0xE9
    struct.pack_into("<I", payload, 32, 0xABCD5432)
    payload[48:48 + len(version)] = version.encode()
    partitions = [(0, 0 if label == "factory" else 16, label,
                   0x10000 * (i + 1), 0x10000, payload) for i, label in enumerate(apps)]
    with tempfile.TemporaryDirectory() as folder:
        bootloader = Path(folder) / "bootloader.bin"
        bootloader.write_bytes(b"\xe9\x00")
        return mkfw.create_image("esp32s3", partitions, str(bootloader), "Retro-Go", version, target)


class ReleaseTests(unittest.TestCase):
    def test_release_changelog(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "CHANGELOG.md").write_text(
                "# Retro-Go 4.5\n\n## Updates\n\n- New updater.\n\n# Retro-Go 4.4\n\n- Old change.\n")
            with patch.object(release, "ROOT", Path(folder)):
                notes = release.release_notes("v4.5")
                self.assertIn("## Updates", notes)
                self.assertIn("New updater.", notes)
                self.assertNotIn("Old change.", notes)

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
            self.assertEqual(len(manifest["assets"]), 8)
            self.assertEqual(len((Path(folder) / "SHA256SUMS").read_text().splitlines()), 8)
            next(Path(folder).glob("*.img")).unlink()
            with self.assertRaises(ValueError): release.package_release(folder, "v4.5")


if __name__ == "__main__":
    unittest.main()
