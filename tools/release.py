#!/usr/bin/env python3
"""Validate release tags/images and write a complete SHA-256 asset manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parent.parent
APPS = ("factory", "launcher", "retro-core", "prboom-go", "gwenesis", "fmsx", "retro-legacy")


def release_version(tag):
    if not re.fullmatch(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*))?", tag):
        raise ValueError("Use a stable tag such as v4.5 or v4.5.1")
    version = tag[1:]
    if len(version) > 28 or any(int(n) > 0xFFFFFFFF for n in version.split(".")):
        raise ValueError("Version exceeds firmware metadata limits")
    return version


def release_targets():
    targets = json.loads((ROOT / "tools/release-targets.json").read_text())
    if not targets or len(set(targets)) != len(targets):
        raise ValueError("Release targets must be nonempty and unique")
    for target in targets:
        if len(target.encode()) > 28 or not (ROOT / "components/retro-go/targets" / target / "config.h").is_file():
            raise ValueError(f"Invalid image target: {target}")
    return targets


def validate_image(path, version, target):
    data = Path(path).read_bytes()
    if len(data) <= 0x10000 + 256:
        raise ValueError(f"{path}: truncated image")
    magic, name, embedded_version, embedded_target, timestamp, crc, _ = struct.unpack(
        "<8s28s28s28sII156s", data[-256:])
    decode = lambda value: value.rstrip(b"\0").decode("ascii")
    if (magic, decode(name), decode(embedded_version), decode(embedded_target)) != (
            b"RG_IMG_0", "Retro-Go", version, target):
        raise ValueError(f"{path}: image identity/version/target mismatch")
    image = data[:-256]
    if zlib.crc32(image) != crc:
        raise ValueError(f"{path}: CRC mismatch")
    capacity = 16 * 1024 * 1024 if "-n16r8-" in target else 8 * 1024 * 1024
    if len(data) > capacity:
        raise ValueError(f"{path}: image exceeds flash capacity")
    apps = []
    end = 0x9000
    for offset in range(0x8000, 0x8C00, 32):
        entry = image[offset:offset + 32]
        if entry[:2] != b"\xaa\x50":
            break
        kind, subtype = entry[2:4]
        start, size = struct.unpack_from("<II", entry, 4)
        label = decode(entry[12:28])
        if start < end or size == 0 or start + size > len(image):
            raise ValueError(f"{path}: invalid partition {label}")
        end = start + size
        if kind == 0:
            if start % 0x10000 or size % 0x10000 or image[start] != 0xE9:
                raise ValueError(f"{path}: invalid app partition {label}")
            if label == "factory" and subtype != 0:
                raise ValueError(f"{path}: factory must use the factory subtype")
            # ESP-IDF app descriptor follows the image and first segment headers.
            if struct.unpack_from("<I", image, start + 32)[0] != 0xABCD5432 or \
                    decode(image[start + 48:start + 80]) != version:
                raise ValueError(f"{path}: {label} was built with a different version")
            apps.append(label)
    if tuple(apps) != APPS:
        raise ValueError(f"{path}: missing or unexpected applications: {apps}")
    return {"target": target, "filename": Path(path).name, "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "version": version, "timestamp": timestamp}


def package_release(directory, tag):
    version = release_version(tag)
    directory = Path(directory)
    expected = {f"retro-go_{version}_{target}.img" for target in release_targets()}
    actual = {path.name for path in directory.glob("*.img")}
    if actual != expected:
        raise ValueError(f"Release images differ: missing={expected - actual}, unexpected={actual - expected}")
    assets = [validate_image(directory / f"retro-go_{version}_{target}.img", version, target)
              for target in release_targets()]
    (directory / "SHA256SUMS").write_text("".join(
        f"{asset['sha256']}  {asset['filename']}\n" for asset in assets), encoding="utf-8")
    (directory / "release-manifest.json").write_text(json.dumps({
        "schema_version": 1, "tag": tag, "version": version,
        "commit": os.getenv("GITHUB_SHA"), "assets": assets}, indent=2) + "\n", encoding="utf-8")


def release_notes(tag):
    version = release_version(tag)
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    match = re.search(r"^# Retro-Go " + re.escape(version) + r"(?:[ \t][^\n]*)?\n(.*?)(?=^# |\Z)",
                      changelog, re.MULTILINE | re.DOTALL)
    changes = match.group(1).strip() + "\n\n" if match else ""
    return changes + (
        "Images include the factory updater and all emulator apps. Devices automatically select "
        "the image for their exact hardware target from the latest stable release.\n\n"
        "For a first installation, choose the image matching your board and flash it over USB. "
        "For later updates, connect Wi-Fi and use **Check for updates** in the launcher. "
        "Keep the device powered during installation.\n\n"
        "`SHA256SUMS` and `release-manifest.json` contain checksums and image metadata.\n\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("targets")
    sub.add_parser("version").add_argument("tag")
    sub.add_parser("notes").add_argument("tag")
    check = sub.add_parser("check")
    check.add_argument("image")
    check.add_argument("version")
    check.add_argument("target")
    package = sub.add_parser("package")
    package.add_argument("directory")
    package.add_argument("tag")
    args = parser.parse_args()
    if args.command == "targets": print(json.dumps(release_targets()))
    elif args.command == "version": print(release_version(args.tag))
    elif args.command == "notes": print(release_notes(args.tag), end="")
    elif args.command == "check": print(json.dumps(validate_image(args.image, args.version, args.target)))
    else: package_release(args.directory, args.tag)


if __name__ == "__main__":
    main()
