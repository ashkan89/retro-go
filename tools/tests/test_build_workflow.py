"""Run the Actions packaging script, substituting only ESP-IDF compilation."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]


class BuildWorkflowTests(unittest.TestCase):
    def test_default_apps_produce_valid_release_images(self):
        bash = shutil.which("bash")
        if os.name == "nt" and shutil.which("git"):
            git_bash = Path(shutil.which("git")).resolve().parents[1] / "bin/bash.exe"
            if git_bash.is_file():
                bash = str(git_bash)
        if not bash:
            self.skipTest("The Actions build step requires bash")

        workflow = (ROOT / ".github/workflows/build-images.yml").read_text()
        block = re.search(
            r"(?m)^      - name: Build complete image with factory updater\n"
            r"(?:        .*\n)*?        run: \|\n((?:          .*\n)+)", workflow)
        self.assertIsNotNone(block, "Missing firmware build step")
        script = textwrap.dedent(block[1])
        for target in ("esp32-p4-n16r32-ili9341", "esp32-p4-n16r32-st7796",
                       "esp32-s3-n16r8-ili9341", "esp32-s3-n8r2-st7796",
                       "esp32-s3-n8r2-ili9341"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / "tools").mkdir()
                shutil.copy2(ROOT / "rg_tool.py", root)
                for tool in ("mkfw.py", "release.py"):
                    shutil.copy2(ROOT / "tools" / tool, root / "tools")
                target_dir = root / "components/retro-go/targets" / target
                shutil.copytree(ROOT / "components/retro-go/targets" / target, target_dir)
                for app in ("factory", "launcher", "retro-core", "prboom-go",
                            "gwenesis", "fmsx", "retro-legacy", "gbsp"):
                    (root / app).mkdir()

                idf = root / "idf"
                (idf / "tools").mkdir(parents=True)
                (idf / "export.sh").write_text('export PATH="$IDF_PATH/tools:$PATH"\n')
                compiler = idf / "tools/idf.py"
                compiler.write_text(textwrap.dedent('''\
                    #!/usr/bin/env python3
                    import os
                    from pathlib import Path
                    import struct
                    app = Path.cwd().name
                    build = Path("build")
                    build.mkdir(exist_ok=True)
                    payload = bytearray(256)
                    if os.environ["BUILD_TARGET"] == "esp32-s3-n8r2-ili9341":
                        # Reproduce the release overflow: two app reservations
                        # grow by a 64 KiB block, exhausting the footer space.
                        size = {"factory": 0x90001, "launcher": 0x170001}.get(app, 256)
                        payload = bytearray(size)
                    payload[0] = 0xE9
                    struct.pack_into("<I", payload, 32, 0xABCD5432)
                    version = os.environ["PROJECT_VER"].encode()
                    payload[48:48 + len(version)] = version
                    (build / (app + ".bin")).write_bytes(payload)
                    (build / "bootloader").mkdir(exist_ok=True)
                    (build / "bootloader/bootloader.bin").write_bytes(b"\\xe9\\x00")
                    '''))
                compiler.chmod(0o755)
                env = dict(os.environ, IDF_PATH=idf.as_posix(), BUILD_TARGET=target,
                           PROJECT_VER="0.0.0", PROJECT_NAME="Retro-Go")
                env["PATH"] = str(Path(sys.executable).parent) + os.pathsep + env["PATH"]
                result = subprocess.run([bash, "-eo", "pipefail", "-c", script],
                                        cwd=root, env=env, capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((root / f"retro-go_0.0.0_{target}.img").is_file())


if __name__ == "__main__":
    unittest.main()
