import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from retro_go_sd_backup import RetroGoClient


class SDWebClientTests(unittest.TestCase):
    def test_lists_all_pages_and_adds_mutation_header(self):
        calls = []

        def open_request(request, **kwargs):
            del kwargs
            self.assertEqual(request.get_header("X-retrogo"), "1")
            data = json.loads(request.data)
            calls.append(data)
            offset = data.get("offset", 0)
            return io.BytesIO(json.dumps({"success": True, "files": [{"name": "second" if offset else "first"}],
                                          "truncated": not offset, "next_offset": 1 if not offset else 2}).encode())

        with patch("urllib.request.urlopen", side_effect=open_request):
            result = RetroGoClient("http://localhost").list_dir("/sd")
        self.assertEqual([entry.name for entry in result], ["first", "second"])
        self.assertEqual(calls[1]["offset"], 1)

    def test_upload_explicitly_preserves_restore_overwrite_semantics(self):
        def open_request(request, **kwargs):
            del kwargs
            self.assertEqual(request.method, "PUT")
            self.assertEqual(request.get_header("X-retrogo"), "1")
            self.assertEqual(request.get_header("X-retrogo-overwrite"), "1")
            self.assertEqual(request.data.read(), b"saved data")
            return io.BytesIO(b"OK")

        client = RetroGoClient("http://localhost")
        with patch.object(client, "ensure_dir"), patch("urllib.request.urlopen", side_effect=open_request):
            client.put_bytes("/sd/retro-go/saves/game.sav", b"saved data")


if __name__ == "__main__":
    unittest.main()
