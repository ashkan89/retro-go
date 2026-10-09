"""Run native media audio, transition, MP3 trimming, geometry and LED regression tests.

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


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{": depth += 1
        if source[end] == "}": depth -= 1
        end += 1
    return source[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--preview-dir", type=Path, help="Write host-rendered Now Playing PPM previews")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="retro-media-") as directory:
        work = Path(directory)
        (work / "rg_system.h").write_text((root / "tools/tests/media_audio_stubs.h").read_text(encoding="utf-8"), encoding="utf-8")
        player = (root / "components/media/media_player.c").read_text(encoding="utf-8")
        state = player[player.index("static void set_state("):player.index("static void set_error(")]
        buffering = player[player.index("static bool prebuffer_satisfied("):
                           player.index("static bool prepared_done(")]
        (work / "media_buffering_under_test.h").write_text(state + buffering, encoding="utf-8")
        media = (root / "components/media/media.c").read_text(encoding="utf-8")
        (work / "media_tick_under_test.h").write_text(
            media[media.index("void media_tick("):media.index("bool media_has_library(")], encoding="utf-8")
        (work / "media_route_under_test.h").write_text(
            player[player.index("static void poll_audio_route("):player.index("void media_player_tick(")], encoding="utf-8")
        (work / "media_prepared_type.h").write_text(
            player[player.index("typedef struct\n{"):player.index("static struct\n{")], encoding="utf-8")
        (work / "media_continuous_under_test.h").write_text(
            function(player, "static void configure_tail(") +
            player[player.index("static void track_gain("):player.index("static void decode_task(")], encoding="utf-8")
        queue = (root / "components/media/media_queue.c").read_text(encoding="utf-8")
        (work / "media_queue_under_test.h").write_text("\n".join(function(queue, signature)
            for signature in ["void media_queue_reshuffle(", "int media_queue_next_index(", "int media_queue_advance("]), encoding="utf-8")
        (work / "media_rand_under_test.h").write_text(function(
            (root / "components/media/media_util.c").read_text(encoding="utf-8"), "uint32_t media_rand("), encoding="utf-8")
        system = (root / "components/retro-go/rg_system.c").read_text(encoding="utf-8")
        (work / "media_led_under_test.h").write_text(function(system, "static void update_indicators(") +
            function(system, "void rg_system_set_led_override("), encoding="utf-8")
        (work / "media_task_slots_under_test.h").write_text(function(system, "static rg_task_t *task_reserve(") +
            function(system, "static void task_release("), encoding="utf-8")
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
        subprocess.run([str(executable)], check=True, timeout=60, cwd=root)
        if args.preview_dir:
            # Run real layout/drawing functions with real Sans glyphs and a host framebuffer.
            ui = (root / "components/media/media_ui.c").read_text(encoding="utf-8")
            (work / "media_preview_helpers.h").write_text("\n".join(function(ui, "void " + name + "(")
                for name in ["media_ui_draw_panel", "media_ui_draw_progress", "media_ui_draw_header",
                             "media_ui_draw_footer", "media_ui_draw_marquee"]), encoding="utf-8")
            screens = (root / "components/media/media_ui_player.c").read_text(encoding="utf-8")
            (work / "media_preview_player.h").write_text(screens[screens.index("static const char *track_title("):
                screens.index("void media_ui_lyrics_draw(")], encoding="utf-8")
            (work / "media_preview_font.h").write_text("\n".join(
                (root / f"components/retro-go/fonts/{font}.c").read_text(encoding="utf-8").replace('#include "../rg_gui.h"', '')
                for font in ["Sans12", "Sans15"]), encoding="utf-8")
            (work / "media_preview_glyph.h").write_text(function(
                (root / "components/retro-go/rg_gui.c").read_text(encoding="utf-8"), "static size_t get_glyph("), encoding="utf-8")
            preview_command = command.copy()
            preview_command += ["-Wno-unused-function", "-Wno-sign-compare"]
            preview_command[preview_command.index(str(root / "tools/tests/test_media_audio.c"))] = str(
                root / "tools/tests/media_ui_preview.c")
            subprocess.run(preview_command, check=True)
            args.preview_dir.mkdir(parents=True, exist_ok=True)
            for width, height, font in [(320,240,16),(480,320,16),(240,320,16),(320,240,19)]:
                path = (args.preview_dir / f"player-{width}x{height}-font{font}.ppm").resolve()
                subprocess.run([str(executable), str(width), str(height), str(path), str(font)], check=True, cwd=root)
            print(f"Host UI previews written to {args.preview_dir}")
        feature_command = command.copy()
        feature_command[feature_command.index(str(root / "tools/tests/test_media_audio.c"))] = str(
            root / "tools/tests/test_media_features.c")
        feature_command += ["-Wno-unused-function"]
        subprocess.run(feature_command, check=True)
        subprocess.run([str(executable)], check=True, timeout=60, cwd=root)
        # Exercise actual target headers, including the non-launcher display default.
        config_test = work / "affinity.c"
        config_test.write_text('''#include "config.h"
_Static_assert(RG_TASK_AFFINITY_MAIN == 0, "UI core");
_Static_assert(RG_TASK_AFFINITY_AUDIO == 1, "audio core");
_Static_assert(RG_TASK_AFFINITY_DISPLAY == EXPECT_DISPLAY, "display core");
''', encoding="utf-8")
        compiler = command[:2] if Path(args.cc).stem.lower() == "zig" else command[:1]
        targets = json.loads((root / "tools/release-targets.json").read_text(encoding="utf-8"))
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
