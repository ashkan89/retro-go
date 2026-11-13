"""Complete and validate standalone Retro-Go themes.

Check: python tools/theme_assets.py --check
Build missing vector artwork/previews: pip install resvg-py; python tools/theme_assets.py --complete
Existing tab artwork is preserved. The SVG templates below are the editable
source for the new UI glyphs; no commercial console artwork is downloaded.
"""
import argparse
import base64
from html import escape
import json
from pathlib import Path
import re
import shutil

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
THEMES = ROOT / "themes"
LABELS = {
    "nes": "Nintendo NES", "snes": "Super Nintendo", "gb": "Game Boy",
    "gbc": "Game Boy Color", "gba": "Game Boy Advance", "gw": "Game & Watch",
    "sms": "Master System", "gg": "Game Gear", "md": "Mega Drive",
    "col": "ColecoVision", "pce": "PC Engine", "lnx": "Atari Lynx",
    "spectrum": "ZX Spectrum", "a26": "Atari 2600", "a78": "Atari 7800",
    "doom": "DOOM", "msx": "MSX", "favorite": "Favorites",
    "recent": "Recently Played", "mediaplayer": "Media Player",
    "browser": "File Manager", "collections": "Collections", "themes": "Themes",
}
SIZES = {"background": (320, 240), "banner": (272, 24), "logo": (46, 50)}
ALIASES = {"a26": "2600", "a78": "7800", "col": "coleco"}
FOLDERS = {"Commic Book": "comicbook", "Super Lopez": "Superlopez"}
DIALOG_KEYS = "background foreground border header scrollbar shadow item_standard item_disabled item_message item_value accent accent_dim highlight surface surface_alt divider text_dim".split()
LAUNCHER_KEYS = "background foreground list_standard_bg list_standard_fg list_selected_bg list_selected_fg".split()
MEDIA_KEYS = "background surface text text_dim divider accent accent_dim highlight".split()


def rgb(value):
    number = int(value, 16)
    if len(value) == 6:  # RGB565, as accepted by the firmware
        return ((number >> 11) * 255 // 31, ((number >> 5) & 63) * 255 // 63, (number & 31) * 255 // 31)
    return ((number >> 16) & 255, (number >> 8) & 255, number & 255)


def quantized(value):
    r, g, b = rgb(value)
    return (r >> 3 << 3, g >> 2 << 2, b >> 3 << 3)


def luminance(color):
    channels = [x / 255 for x in color]
    return sum(w * (x / 12.92 if x <= .04045 else ((x + .055) / 1.055) ** 2.4)
               for w, x in zip((.2126, .7152, .0722), channels))


def contrast(a, b):
    low, high = sorted((luminance(quantized(a)), luminance(quantized(b))))
    return (high + .05) / (low + .05)


def readable(value, background, minimum=4.5):
    original = rgb(value)
    target = (255, 255, 255) if luminance(rgb(background)) < .4 else (0, 0, 0)
    for step in range(101):
        color = "0x%02X%02X%02X" % tuple(round(a + (b - a) * step / 100) for a, b in zip(original, target))
        if contrast(color, background) >= minimum:
            return color
    return color


def css(color):
    return "#%02x%02x%02x" % rgb(color)


def rect(x, y, w, h, radius=0, extra=""):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" {extra}/>'


def glyph(tab):
    cross = '<path d="M11 28h5v-5h5v5h5v5h-5v5h-5v-5h-5z"/>'
    if tab in ("gb", "gbc", "gw"):
        return rect(9, 5, 28, 40, 5) + rect(14, 10, 18, 14, 1) + '<path d="M13 33h10m-5-5v10"/><circle cx="28" cy="31" r="1.4"/><circle cx="32" cy="28" r="1.4"/>'
    if tab in ("gba", "gg", "lnx"):
        return rect(3, 11, 40, 28, 10) + rect(14, 16, 18, 18, 2) + '<path d="M5 25h7m-3-4v8"/><circle cx="37" cy="22" r="1.8"/><circle cx="36" cy="29" r="1.8"/>'
    if tab in ("spectrum", "msx"):
        return rect(3, 13, 40, 25, 3) + '<path d="M7 19h31M7 24h31M7 29h31M11 17v14m6-14v14m6-14v14m6-14v14m6-14v14M14 34h18"/>'
    if tab == "favorite":
        return '<path d="m23 7 5 12 13 1-10 9 3 14-11-7-11 7 3-14-10-9 13-1z"/>'
    if tab == "recent":
        return '<circle cx="23" cy="25" r="17"/><path d="M23 13v13l9 5M5 14v10h10"/>'
    if tab == "mediaplayer":
        return '<path d="M18 35V13l21-5v23M18 20l21-5"/><ellipse cx="12" cy="36" rx="6" ry="4"/><ellipse cx="33" cy="32" rx="6" ry="4"/>'
    if tab in ("browser", "collections"):
        return '<path d="M4 15h15l4 5h19v21H4z"/>' + (rect(9, 25, 9, 10, 1) + rect(23, 25, 9, 10, 1) if tab == "collections" else '<path d="M11 28h24m-24 6h17"/>')
    if tab == "themes":
        return '<path d="M23 7C1 6 0 42 20 43c12 0 4-12 13-12C51 31 40 6 23 7z"/><circle cx="14" cy="18" r="2"/><circle cx="25" cy="13" r="2"/><circle cx="34" cy="20" r="2"/><circle cx="11" cy="29" r="2"/>'
    if tab == "doom":
        return '<path d="m9 35-2-17L15 8h16l8 10-2 17-9 8H18zM12 21h22v9H12zM18 35h10M23 30v10"/>'
    if tab in ("a26", "a78"):
        return '<path d="M8 16h30l5 21H3zM10 20h26M9 24h28M15 28h16M13 13v-3m7 3v-3m7 3v-3m7 3v-3"/>' + rect(9, 32, 6, 2) + rect(31, 32, 6, 2)
    if tab in ("snes", "md", "sms"):
        return '<path d="M13 14h20c9 0 12 20 6 23-4 3-9-5-12-5h-8c-3 0-8 8-12 5-6-3-3-23 6-23zM7 23h12m-6-6v12"/><circle cx="33" cy="21" r="2"/><circle cx="37" cy="26" r="2"/><circle cx="29" cy="26" r="2"/><circle cx="33" cy="31" r="2"/>'
    return rect(4, 11, 38, 29, 3) + rect(10, 16, 26, 9, 1) + '<path d="M9 33h9m-5-4v8"/><circle cx="31" cy="32" r="2"/><circle cx="37" cy="32" r="2"/>'


def svg(body, size):
    w, h = size
    return f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{w}" height="{h}" viewBox="0 0 {w} {h}">{body}</svg>'


def image_tag(path, x, y, w, h):
    data = base64.b64encode(path.read_bytes()).decode()
    return f'<image x="{x}" y="{y}" width="{w}" height="{h}" preserveAspectRatio="xMidYMid slice" xlink:href="data:image/png;base64,{data}"/>'


def text(value, x, y, color, size=14, weight=600):
    return f'<text x="{x}" y="{y}" fill="{color}" font-family="Segoe UI, DejaVu Sans, sans-serif" font-size="{size}" font-weight="{weight}">{escape(value)}</text>'


def render(body, size, path):
    import resvg_py
    # Opaque backgrounds/color keys are intentional: firmware decodes PNG to RGB24.
    path.write_bytes(resvg_py.svg_to_bytes(svg_string=svg(body, size)))


def artwork(kind, tab, config):
    pal = config["dialog"]
    accent, surface, foreground = map(css, (pal["accent"], pal["surface"], pal["foreground"]))
    background = css(config["launcher_1"]["background"])
    mark = f'<g fill="none" stroke="{accent}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">{glyph(tab)}</g>'
    if kind == "logo":
        return rect(0, 0, 46, 50, 0, f'fill="{surface}"') + mark
    if kind == "banner":
        label = LABELS.get(tab, {"ngp": "Neo Geo Pocket"}.get(tab, tab.upper()))
        return rect(0, 0, 272, 24, 0, 'fill="#ff00ff"') + rect(0, 5, 3, 14, 1, f'fill="{accent}"') + text(label, 12, 18, foreground, 17)
    return (rect(0, 0, 320, 240, 0, f'fill="{background}"') +
            f'<path d="M235 0v42l36 36v74l49 49M256 0v35l34 34v75l30 30M0 224h186l18-18h116" fill="none" stroke="{accent}" stroke-opacity=".13" stroke-width="1"/>' +
            f'<g transform="translate(227 132) scale(1.7)" opacity=".08">{mark}</g>')


def improve_config(config):
    defaults = json.loads((THEMES / "default/theme.json").read_text())
    for section, keys in [("dialog", DIALOG_KEYS), ("media", MEDIA_KEYS)] + [(f"launcher_{i}", LAUNCHER_KEYS) for i in range(1, 5)]:
        values = config.setdefault(section, {})
        for key in keys:
            values.setdefault(key, defaults[section][key])
    pal = config["dialog"]
    for key in ("foreground", "header", "item_standard", "item_message", "item_value", "text_dim"):
        pal[key] = readable(pal[key], pal["surface"])
    pal["highlight"] = readable(pal["highlight"], pal["accent_dim"])
    for key in ("text", "text_dim"):
        config["media"][key] = readable(config["media"][key], config["media"]["surface"])
    for index in range(1, 5):
        values = config[f"launcher_{index}"]
        values["list_standard_fg"] = readable(values["list_standard_fg"], values["background"])
        bg = values["list_selected_bg"]
        values["list_selected_fg"] = readable(values["list_selected_fg"], pal["accent_dim"] if bg == "transparent" else bg)
    return config


def preview(folder, config):
    pal = config["dialog"]
    bg, fg, accent, surface, selected, dim = map(css, (pal["background"], pal["foreground"], pal["accent"], pal["surface"], pal["accent_dim"], pal["text_dim"]))
    body = image_tag(folder / "background_gba.png", 0, 0, 320, 240)
    body += rect(0, 0, 320, 43, 0, f'fill="{bg}"') + rect(0, 42, 320, 1, 0, f'fill="{accent}"')
    body += image_tag(folder / "logo_gba.png", 9, 4, 31, 34) + text("Game Boy Advance", 49, 24, fg, 16)
    body += rect(10, 59, 300, 138, 8, f'fill="{bg}" fill-opacity=".94"')
    body += text("YOUR LIBRARY", 23, 80, dim, 10)
    body += rect(19, 91, 282, 28, 5, f'fill="{selected}"') + rect(19, 96, 3, 18, 1, f'fill="{accent}"')
    body += text("Adventure.gba", 31, 110, css(pal["highlight"]), 14)
    body += text("Puzzle.gba", 31, 140, fg, 14) + text("Racing.gba", 31, 169, fg, 14)
    body += rect(0, 214, 320, 26, 0, f'fill="{surface}"')
    body += text(folder.name, 11, 231, fg, 11) + text("A  PLAY     B  BACK", 200, 231, fg, 9)
    # Render at device resolution, then let resvg export an exact selector thumbnail.
    import resvg_py
    (folder / "preview.png").write_bytes(resvg_py.svg_to_bytes(svg_string=svg(body, (320, 240)), width=160, height=120))


def placeholder(path):
    with Image.open(path) as image:
        colors = image.convert("RGB").getcolors(image.width * image.height)
        return len(colors) == 1 or sum(count for count, color in colors if color != (255, 0, 255)) < 10


def complete():
    created = repaired = 0
    for folder in sorted(THEMES.iterdir()):
        if not folder.is_dir():
            continue
        config_path = folder / "theme.json"
        config = improve_config(json.loads(config_path.read_text(encoding="utf-8-sig")) if config_path.exists() else {"description": folder.name, "author": "Retro-Go"})
        config_path.write_text(json.dumps(config, indent=4, ensure_ascii=False) + "\n", encoding="utf-8")
        donor = THEMES / FOLDERS.get(folder.name, folder.name)
        if donor != folder:
            for asset in donor.glob("*.png"):
                if not (folder / asset.name).exists():
                    shutil.copy2(asset, folder / asset.name)
                    created += 1
        for tab in LABELS:
            for kind, size in SIZES.items():
                dest = folder / f"{kind}_{tab}.png"
                if dest.exists():
                    continue
                alias = folder / f"{kind}_{ALIASES.get(tab, tab)}.png"
                if alias.exists():
                    shutil.copy2(alias, dest)
                elif folder.name == "classic" and kind == "background":
                    art = ROOT / "assets/theme-art/classic-background.png"
                    render(image_tag(art, 0, 0, *size), size, dest)
                else:
                    render(artwork(kind, tab, config), size, dest)
                created += 1
        # Every bundle also has an explicit fallback and selector thumbnail.
        fallback = folder / "background.png"
        if folder.name == "classic":
            shutil.copy2(folder / "background_gba.png", fallback)
        elif not fallback.exists():
            render(artwork("background", "themes", config), SIZES["background"], fallback)
        # Some older themes used a blank PNG to suppress a built-in logo/title.
        # A file-existence check alone would mistake these for complete media.
        for path in sorted(folder.glob("*.png")):
            if path.name.startswith(("banner_", "logo_")) and placeholder(path):
                kind, tab = path.stem.split("_", 1)
                tab = {value: key for key, value in ALIASES.items()}.get(tab, tab)
                render(artwork(kind, tab, config), SIZES[kind], path)
                repaired += 1
        preview(folder, config)
    print(f"Completed {created} missing assets, repaired {repaired} blank placeholders; refreshed configs and selector previews")


def validate():
    problems = []
    count = images = 0
    runtime = set(re.findall(r'^\s*application\("[^"]+", "([^"]+)"', (ROOT / "launcher/main/applications.c").read_text(), re.M))
    if runtime - LABELS.keys():
        problems.append(f"Missing tab definitions: {runtime - LABELS.keys()}")
    if (ROOT / "launcher/main/images.c").exists():
        problems.append("Launcher artwork must be installed on SD, not regenerated into images.c")
    for folder in sorted(THEMES.iterdir()):
        if not folder.is_dir():
            continue
        count += 1
        try:
            config = json.loads((folder / "theme.json").read_text(encoding="utf-8-sig"))
            for section, keys in [("dialog", DIALOG_KEYS), ("media", MEDIA_KEYS)] + [(f"launcher_{i}", LAUNCHER_KEYS) for i in range(1, 5)]:
                for key in keys:
                    value = config.get(section, {}).get(key)
                    if not isinstance(value, str) or not re.fullmatch(r"0x(?:[0-9a-fA-F]{4}|[0-9a-fA-F]{6})|transparent|none", value):
                        problems.append(f"{folder.name}: invalid/missing {section}.{key}: {value}")
            pairs = [(config["dialog"]["highlight"], config["dialog"]["accent_dim"]),
                     (config["dialog"]["item_standard"], config["dialog"]["surface"]),
                     (config["media"]["text_dim"], config["media"]["surface"])]
            if any(contrast(a, b) < 4.5 for a, b in pairs):
                problems.append(f"{folder.name}: text contrast below 4.5 after RGB565 quantization")
        except (ValueError, KeyError, OSError) as exc:
            problems.append(f"{folder.name}: {exc}")
        required = ["background.png", "preview.png"] + [f"{kind}_{tab}.png" for tab in LABELS for kind in SIZES]
        for name in required:
            if not (folder / name).is_file():
                problems.append(f"{folder.name}: missing {name}")
        for path in folder.glob("*.png"):
            try:
                with Image.open(path) as image:
                    image.load()
                    if image.format != "PNG" or image.width == 0 or image.height == 0:
                        problems.append(f"Invalid PNG: {path}")
                    if path.name == "preview.png" and image.size != (160, 120):
                        problems.append(f"Incorrect preview dimensions: {path}")
                    if image.width > 320 or image.height > 240:
                        problems.append(f"Oversized launcher image: {path}")
                if path.name.startswith(("banner_", "logo_")) and placeholder(path):
                    problems.append(f"Blank artwork placeholder: {path}")
                images += 1
            except OSError as exc:
                problems.append(f"{path}: {exc}")
    if problems:
        raise SystemExit("\n".join(problems))
    print(f"PASS: {count} complete themes, {len(LABELS)} tab sets per theme, {images} valid PNGs, explicit palettes and RGB565 text contrast")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--complete", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.complete:
        complete()
    validate()
