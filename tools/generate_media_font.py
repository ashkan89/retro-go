"""Generate the Latin/Arabic UI font used by the launcher media player.

The player draws small text over album art, so legibility matters more than
compactness. Open Sans was drawn for on-screen UI and holds up far better at
small pixel sizes than DejaVu Sans, whose Bitstream Vera skeleton gets muddy
below about 16px. Open Sans has no Arabic coverage though, so the two are
merged: Open Sans supplies Latin/Latin-1, DejaVu Sans supplies the Arabic
block and the presentation forms the bidi shaper in rg_gui.c maps onto.

Semibold rather than Regular because thin stems disappear against a bright
patch of cover art.
"""

import importlib.util
from pathlib import Path


class Value:
    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value


FONT_SIZE = 15
LATIN_RANGES = "32-255"
# Arabic block, then the presentation forms A/B that the shaper emits.
ARABIC_RANGES = "1536-1791,64336-65023,65136-65279"

root = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("font_converter", root / "font_converter.py")
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)

# Off deliberately. Enforcing the nominal size shoves any glyph whose ink box is
# taller than the cell upwards, which squashes descenders (g, y, p) and clips
# Arabic marks. Letting the converter measure the real extents costs a few
# pixels of line height and keeps every glyph intact.
converter.enforce_font_size_bool = Value(0)


def load(ttf, ranges, size):
    converter.list_char_ranges = Value(ranges)
    name, actual_size, glyphs = converter.load_ttf_font(str(root / "fonts" / ttf), size)
    return name, actual_size, glyphs


latin_name, size, latin = load("OpenSans-Semibold.ttf", LATIN_RANGES, FONT_SIZE)
# DejaVu runs a little large at the same nominal size; a point smaller keeps the
# Arabic x-height in line with the Latin.
arabic_name, _, arabic = load("DejaVuSans.ttf", ARABIC_RANGES, FONT_SIZE - 1)

seen = {glyph["char_code"] for glyph in latin}
merged = latin + [glyph for glyph in arabic if glyph["char_code"] not in seen]
merged.sort(key=lambda glyph: glyph["char_code"])

output = converter.generate_c_font("Media Sans", size, merged)
target = root.parent / "components" / "retro-go" / "fonts" / f"MediaSans{size}.c"
target.write_text(output, encoding="utf-8")

print(f"{latin_name} + {arabic_name} -> {target.name}")
print(f"  {len(merged)} glyphs ({len(latin)} latin, {len(merged) - len(latin)} arabic), size {size}")
