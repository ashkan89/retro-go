"""Run the optional smoke-test firmware in Espressif QEMU (ESP32-S3)."""
import argparse
import json
import pathlib
import subprocess
import threading

parser = argparse.ArgumentParser()
parser.add_argument("qemu", help="Path to qemu-system-xtensa")
parser.add_argument("--build", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / "build")
parser.add_argument("--timeout", type=int, default=60)
parser.add_argument("--psram-mb", type=int, choices=(2, 8), default=8)
args = parser.parse_args()
config = json.loads((args.build / "config/sdkconfig.json").read_text())
flash_mb = int(config.get("ESPTOOLPY_FLASHSIZE", "16MB").removesuffix("MB"))
image = bytearray(b"\xff" * (flash_mb * 1024 * 1024))
for offset, file in [(0, "bootloader/bootloader.bin"), (0x8000, "partition_table/partition-table.bin"),
                     (0x10000, "retro-legacy.bin")]:
    data = (args.build / file).read_bytes()
    image[offset:offset + len(data)] = data
flash = args.build / "selftest-flash.bin"
flash.write_bytes(image)
command = [args.qemu, "-machine", "esp32s3", "-m", f"{args.psram_mb}M", "-nographic", "-monitor", "none", "-no-reboot",
           "-drive", f"file={flash.as_posix()},if=mtd,format=raw"]
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
output = []
def read_output():
    for line in iter(process.stdout.readline, b""):
        text = line.decode("utf-8", errors="replace")
        output.append(text)
        print(text, end="", flush=True)
        if "SELFTEST COMPLETE" in text or "SELFTEST FAIL" in text:
            process.terminate()
            return
reader = threading.Thread(target=read_output, daemon=True)
reader.start()
try:
    process.wait(timeout=args.timeout)
except subprocess.TimeoutExpired:
    process.kill()
    process.wait()
reader.join(timeout=5)
log = "".join(output)
(args.build / "selftest-qemu.log").write_text(log, encoding="utf-8")
if "SELFTEST COMPLETE" not in log or "SELFTEST FAIL" in log:
    raise SystemExit("Smoke test failed; see selftest-qemu.log")
print("All emulator smoke tests passed.")
