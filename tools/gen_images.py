"""Export launcher artwork and the web interface to an SD-card folder.

python tools/gen_images.py --output build/sdcard-assets
python tools/gen_images.py --extract old-images.c --output build/sdcard-assets

The legacy extractor decodes PNG arrays without executing C code.
"""
import argparse
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]


def extract(source, output):
    decoded = []
    for name, size, body in re.findall(r'static const binfile_t \w+ = \{"([^"]+)", (\d+), \{(.*?)\}\s*\};', source.read_text(), re.S):
        if not re.fullmatch(r"[A-Za-z0-9_-]+\.png", name):
            raise ValueError(f"Unsafe artwork filename: {name}")
        data = bytes(int(value, 16) for value in re.findall(r"\\x([0-9A-Fa-f]{2})", body))
        if len(data) != int(size) or not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError(f"Invalid embedded PNG: {name}")
        decoded.append((name, data))
    if not decoded:
        raise ValueError("No legacy embedded PNG arrays found")
    destination = output / "retro-go/themes/default"
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in decoded:
        path = destination / name
        if path.exists() and path.read_bytes() != data:
            raise ValueError(f"Refusing to overwrite different artwork: {path}")
        path.write_bytes(data)
    print(f"Extracted {len(decoded)} PNGs ({sum(len(data) for _, data in decoded)} bytes) to {destination}")


def export(output):
    count = 0
    for theme in sorted((ROOT / "themes").iterdir()):
        if not theme.is_dir() or not (theme / "theme.json").is_file():
            continue
        destination = output / "retro-go/themes" / theme.name
        destination.mkdir(parents=True, exist_ok=True)
        for asset in theme.iterdir():
            if asset.is_file() and asset.suffix in (".png", ".json"):
                shutil.copy2(asset, destination / asset.name)
                count += 1
    webui = output / "retro-go/webui/index.html"
    webui.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "assets/webui/index.html", webui)
    print(f"Exported {count} theme files and web UI to {output}; copy retro-go/ to the SD root")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/sdcard-assets")
    parser.add_argument("--extract", type=Path, help="Extract only the PNG arrays from a legacy images.c file")
    args = parser.parse_args()
    if args.extract:
        extract(args.extract, args.output)
    else:
        export(args.output)
