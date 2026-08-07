"""Generate the compact Latin/Arabic font used by the launcher media player."""

import importlib.util
from pathlib import Path


class Value:
    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value


root = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("font_converter", root / "font_converter.py")
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)

converter.enforce_font_size_bool = Value(1)
converter.list_char_ranges = Value("32-255,1536-1791,64336-65023,65136-65279")
name, size, glyphs = converter.load_ttf_font(str(root / "fonts" / "DejaVuSans.ttf"), 13)
output = converter.generate_c_font("DejaVu Media", size, glyphs)
(root.parent / "components" / "retro-go" / "fonts" / "DejaVuMedia13.c").write_text(output, encoding="utf-8")
