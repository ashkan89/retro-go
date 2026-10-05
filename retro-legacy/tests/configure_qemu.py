"""Select QEMU's quad PSRAM and DIO flash, or restore the board configuration."""
import argparse
import pathlib

parser = argparse.ArgumentParser()
parser.add_argument("--restore", action="store_true")
args = parser.parse_args()
app = pathlib.Path(__file__).resolve().parents[1]
config = app / "sdkconfig"
backup = app / "build/sdkconfig.production"
active = app / "build/qemu-config.active"
if args.restore:
    config.write_bytes(backup.read_bytes())
    active.unlink(missing_ok=True)
    print("Restored board SDK configuration.")
else:
    if not active.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_bytes(config.read_bytes())
        active.touch()
    text = config.read_text()
    text = text.replace("# CONFIG_SPIRAM_MODE_QUAD is not set", "CONFIG_SPIRAM_MODE_QUAD=y")
    text = text.replace("CONFIG_SPIRAM_MODE_OCT=y", "# CONFIG_SPIRAM_MODE_OCT is not set")
    for mode in ("QIO", "QOUT", "DOUT"):
        text = text.replace(f"CONFIG_ESPTOOLPY_FLASHMODE_{mode}=y", f"# CONFIG_ESPTOOLPY_FLASHMODE_{mode} is not set")
    text = text.replace("# CONFIG_ESPTOOLPY_FLASHMODE_DIO is not set", "CONFIG_ESPTOOLPY_FLASHMODE_DIO=y")
    config.write_text(text)
    print("Selected quad PSRAM and DIO flash for QEMU; board configuration saved in build/sdkconfig.production.")
