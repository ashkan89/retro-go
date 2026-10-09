"""Compile and run the actual media audio, ring, EQ and FFT code with a mock sink.

Usage: python tools/test_media_audio.py --cc /path/to/clang-or-zig
The host tests check sample ordering and control behavior, not physical I2S deadlines.
"""
import argparse
import os
import json
import re
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="retro-media-") as directory:
        work = Path(directory)
        (work / "rg_system.h").write_text((root / "tools/tests/media_audio_stubs.h").read_text())
        player = (root / "components/media/media_player.c").read_text()
        state = player[player.index("static void set_state("):player.index("static void set_error(")]
        buffering = player[player.index("static bool prebuffer_satisfied("):
                           player.index("static void decode_task(")]
        (work / "media_buffering_under_test.h").write_text(state + buffering)
        media = (root / "components/media/media.c").read_text()
        (work / "media_tick_under_test.h").write_text(
            media[media.index("void media_tick("):media.index("bool media_has_library(")])
        (work / "media_route_under_test.h").write_text(
            player[player.index("static void poll_audio_route("):player.index("void media_player_tick(")])
        executable = work / "media-test.exe"
        command = [args.cc]
        if Path(args.cc).stem.lower() == "zig":
            command.append("cc")
        command += ["-std=c11", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                    "-I", str(work), "-I", str(root / "components/media"),
                    str(root / "tools/tests/test_media_audio.c"), "-o", str(executable)]
        if os.name != "nt":
            command += ["-lm", "-pthread"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True, timeout=60)
        # Exercise actual target headers, including the non-launcher display default.
        config_test = work / "affinity.c"
        config_test.write_text('''#include "config.h"
_Static_assert(RG_TASK_AFFINITY_MAIN == 0, "UI core");
_Static_assert(RG_TASK_AFFINITY_AUDIO == 1, "audio core");
_Static_assert(RG_TASK_AFFINITY_DISPLAY == EXPECT_DISPLAY, "display core");
''')
        compiler = command[:2] if Path(args.cc).stem.lower() == "zig" else command[:1]
        targets = json.loads((root / "tools/release-targets.json").read_text())
        for target in targets:
            macro = "RG_TARGET_" + re.sub(r"[^A-Z0-9]", "_", target.upper())
            for separated in (0, 1):
                subprocess.run(compiler + ["-std=c11", "-c", "-o", str(work / "affinity.o"), "-I",
                    str(root / "components/retro-go"), f"-D{macro}=1",
                    f"-DRG_SEPARATE_DISPLAY_AUDIO={separated}",
                    f"-DEXPECT_DISPLAY={1 - separated}", str(config_test)], check=True)
        print(f"Display/audio affinity checks passed for {len(targets)} targets, launcher and emulator defaults.")


if __name__ == "__main__":
    main()
