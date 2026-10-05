"""Check the actual S3 target app lists and image-capacity guard without ESP-IDF."""
import ast
import contextlib
import copy
import io
import math
import os
from pathlib import Path
import re
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
source = ast.parse((ROOT / "rg_tool.py").read_text())
names = {"read_sdkconfig", "parse_size", "check_image_size"}
functions = ast.Module(body=[n for n in source.body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[])
base = next(ast.literal_eval(n.value) for n in source.body if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "PROJECT_APPS" for t in n.targets))
# Recorded full-feature ESP-IDF 5.5.5 binary sizes; exercise real alignment and headroom.
sizes = dict(factory=541440, launcher=1436480, **{"retro-core": 1169632, "prboom-go": 1007328, "gwenesis": 1158432, "fmsx": 826560, "retro-legacy": 1785344})

class Targets(unittest.TestCase):
    def setUp(self):
        self.previous = Path.cwd()
        os.chdir(ROOT)
    def tearDown(self):
        os.chdir(self.previous)
    def context(self, target):
        ctx = dict(os=os, math=math, re=re, PROJECT_APPS=copy.deepcopy(base))
        exec(compile(functions, "rg_tool.py", "exec"), ctx)
        exec((ROOT / "components/retro-go/targets" / target / "env.py").read_text(), ctx)
        return ctx
    def test_full_images_fit_all_eight_boards(self):
        for memory in ("n8r2", "n16r8"):
            for display in ("st7796", "st7789v2", "st7789", "ili9341"):
                target = f"esp32-s3-{memory}-{display}"
                with self.subTest(target=target):
                    ctx = self.context(target)
                    apps = ctx["DEFAULT_APPS"].split()
                    self.assertEqual(set(apps), set(sizes))
                    with patch("os.path.getsize", side_effect=lambda path: sizes[Path(path).stem]), contextlib.redirect_stdout(io.StringIO()):
                        end = ctx["check_image_size"](apps, target)
                    self.assertLessEqual(end, (8 if memory == "n8r2" else 16)*1048576)
    def test_grown_app_is_rejected_before_packing(self):
        target = "esp32-s3-n8r2-st7796"
        ctx = self.context(target)
        with patch("os.path.getsize", return_value=3*1048576):
            with self.assertRaisesRegex(ValueError, "has 8388608 bytes of flash"):
                ctx["check_image_size"](ctx["DEFAULT_APPS"].split(), target)
    def test_optional_storage_cannot_overrun_flash(self):
        target = "esp32-s3-n8r2-ili9341"
        ctx = self.context(target)
        with patch("os.path.getsize", side_effect=lambda path: sizes[Path(path).stem]):
            with self.assertRaises(ValueError):
                ctx["check_image_size"](ctx["DEFAULT_APPS"].split(), target, "1M")
    def test_metadata_footer_cannot_overrun_flash(self):
        target = "esp32-s3-n8r2-ili9341"
        ctx = self.context(target)
        with patch("os.path.getsize", side_effect=lambda path: sizes[Path(path).stem]):
            with self.assertRaises(ValueError):
                ctx["check_image_size"](ctx["DEFAULT_APPS"].split(), target, "128K")
    def test_size_units(self):
        ctx = self.context("esp32-s3-n8r2-ili9341")
        self.assertEqual(ctx["parse_size"]('"8MB"'), 8388608)
        self.assertEqual(ctx["parse_size"]("500K"), 512000)
        self.assertEqual(ctx["parse_size"]("0x10000"), 65536)

    def test_psram_modes_and_external_bss(self):
        for memory in ("n8r2", "n16r8"):
            for display in ("st7796", "st7789v2", "st7789", "ili9341"):
                target = f"esp32-s3-{memory}-{display}"
                with self.subTest(target=target):
                    ctx = self.context(target)
                    settings = ctx["read_sdkconfig"](f"components/retro-go/targets/{target}/sdkconfig")
                    self.assertEqual(settings["CONFIG_SPIRAM"], "y")
                    self.assertEqual(settings["CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY"], "y")
                    self.assertEqual(settings["CONFIG_SPIRAM_MODE_QUAD"], "y" if memory == "n8r2" else "n")
                    self.assertEqual(settings["CONFIG_SPIRAM_MODE_OCT"], "y" if memory == "n16r8" else "n")

if __name__ == "__main__":
    unittest.main()
