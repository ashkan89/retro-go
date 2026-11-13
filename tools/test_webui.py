"""Exercise production Web UI path checks, flash serving and staged file transfers."""
import argparse
import gzip
from pathlib import Path
import subprocess
import tempfile

from pack_webui import pack

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = (ROOT / "launcher/main/webui.c").read_text(encoding="utf-8-sig")

    def section(start, end):
        return source[source.index(start):source.index(end, source.index(start))]

    harness = (ROOT / "tools/tests/webui_host.c").read_text()
    production = section("static esp_err_t error_response", "static esp_err_t send_json")
    production += section("static esp_err_t http_upload_handler", "// Apply controls")
    production += section("static struct { int action", "#define TRANSFER_SIZE")
    production += section("void webui_poll(void)", "void webui_stop(void)")
    harness = harness.replace("/* PRODUCTION_HANDLERS */", production)
    compiler = str(Path(args.cc).resolve()) if Path(args.cc).exists() else args.cc
    command = [compiler] + (["cc"] if Path(compiler).stem == "zig" else [])
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        (folder / "sd").mkdir()
        (folder / "sd/directory").mkdir()
        (folder / "webui.c").write_text(harness)
        pack(ROOT / "launcher/main/webui/index.html", folder / "asset.c")
        executable = folder / "webui-test.exe"
        subprocess.run(command + ["-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                                 str(folder / "webui.c"), str(folder / "asset.c"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], cwd=folder, check=True)
        assert gzip.decompress((folder / "served.gz").read_bytes()) == (ROOT / "launcher/main/webui/index.html").read_bytes()
    print("PASS: flash asset without SD, path checks, staged uploads/rollback, downloads, deferred controls and physical firmware confirmation")


if __name__ == "__main__":
    main()
