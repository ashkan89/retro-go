import gzip
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pack_webui import pack


class WebUIAssetTests(unittest.TestCase):
    def test_pack_is_reproducible_and_lossless(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source = Path(__file__).resolve().parents[2] / "launcher/main/webui/index.html"
            pack(source, folder / "one.c")
            pack(source, folder / "two.c")
            self.assertEqual((folder / "one.c").read_bytes(), (folder / "two.c").read_bytes())
            data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", (folder / "one.c").read_text()))
            self.assertEqual(gzip.decompress(data), source.read_bytes())
            self.assertEqual(data[9], 255)
            self.assertLessEqual(len(data), 32768)


if __name__ == "__main__":
    unittest.main()
