"""The game folder's cache/ and logs/ (host/game_layout.c), on Linux with the POSIX shim: a game folder a build from before left
its caches and logs in, loose beside the game, started once. No game: authored placeholder files only.

- cache/, cache/prompts and logs/ are made;
- the logs go into logs/, the shader and pipeline caches into cache/, the Vulkan pipeline cache renamed from mkw_vk_pipelines* to
  longshot_vk_pipelines* (its stamp and its play files too), and what a write of it cut short left (.tmp, .tmp-<n>) is deleted;
- the program cache's old lock files beside the game are deleted (the game locks cache/'s);
- the button prompts an older build made in prompts/ are deleted (made again in cache/prompts); the build's own prompts stay;
- the build's files, the settings and the saves stay where they are;
- a file already in cache/ or logs/ is never written over: the loose one stays, and the log says so;
- a second start changes nothing; the game in bin/ beside image.bin finds the folder above it.

python -I -B tests/test_game_layout.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "builds host/game_layout.c with the Linux POSIX shim"}
ROOT = Path(__file__).resolve().parents[1]

MAIN = r'''
#include <stdarg.h>
#include <stdio.h>
#include "game_layout.h"
void hos_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("log: "); vprintf(fmt, ap); printf("\n"); va_end(ap); }
int main(void) {
    hos_game_layout();
    hos_game_layout();   /* (once a process: the second call does nothing) */
    hos_game_layout_log();
    return 0;
}
'''

LOGS = ("longshot_log.txt", "longshot_log.1.txt", "longshot_log.4.txt", "longshot_frames.csv")
CACHES = ("longshot_programs.bin", "longshot_programs.idx", "longshot_pipelines.bin", "longshot_pipelines.bin.built",
          "longshot_computes.bin", "longshot_pipeline_states.bin", "longshot_prepared.bin", "longshot_driver_faults.txt",
          "longshot_load_start.txt")
RENAMED = {"mkw_vk_pipelines.bin": "longshot_vk_pipelines.bin", "mkw_vk_pipelines_play_3.bin": "longshot_vk_pipelines_play_3.bin",
           "mkw_vk_pipelines.stamp": "longshot_vk_pipelines.stamp"}
CUT_SHORT = ("mkw_vk_pipelines.bin.tmp", "mkw_vk_pipelines_play_4.bin.tmp", "mkw_vk_pipelines.bin.tmp-a1B2c3")
OLD_LOCKS = ("longshot_programs.bin.lock", "longshot_programs.zst.bin.lock")
MADE_AGAIN = ("prompts/Common.bntx.zs", "prompts/Common.key", "prompts/External.bfotf.zs", "prompts/rsizetable.mod.zs")
STAY = ("image.bin", "language.txt", "longshot_options.ini", "longshot_env.txt", "longshot_shaders.bin",
        "longshot_pipeline_states_shipped.bin", "prompts/Common.bntx", "prompts/External.bfotf", "prompts/rsizetable.zs",
        "prompts/xbox/textures.bin", "user/save/0100/file")


def game_folder(root, built, in_bin=False):
    """A game folder from before: the build's files, the settings, the saves, and its caches and logs loose beside the game"""
    for name in STAY + LOGS + CACHES + tuple(RENAMED) + CUT_SHORT + MADE_AGAIN + OLD_LOCKS:
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_text(name)
    where = root / "bin" if in_bin else root
    where.mkdir(exist_ok=True)
    exe = where / "longshot"
    shutil.copy2(built, exe)
    return exe


def check(game, out):
    for d in ("cache", "cache/prompts", "logs"):
        assert (game / d).is_dir(), d
    for name in LOGS:
        assert (game / "logs" / name).read_text() == name and not (game / name).exists(), name
    for name in CACHES:
        assert (game / "cache" / name).read_text() == name and not (game / name).exists(), name
    for old, new in RENAMED.items():
        assert (game / "cache" / new).read_text() == old and not (game / old).exists(), old
    assert not list(game.glob("mkw_vk_pipelines*")), list(game.glob("mkw_vk_pipelines*"))
    for name in MADE_AGAIN + OLD_LOCKS:
        assert not (game / name).exists(), name
    for name in STAY:
        assert (game / name).read_text() == name, name
    assert not (game / "cache" / "longshot_options.ini").exists() and not (game / "cache" / "user").exists()
    moved = len(LOGS) + len(CACHES) + len(RENAMED)
    assert ("%d logs and shader caches moved" % moved) in out, out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="longshot-game-layout-") as tmp:
        tmp = Path(tmp)
        (tmp / "main.c").write_text(MAIN)
        built = tmp / "layout"
        inc = ["-std=gnu11", "-I", str(ROOT / "host" / "posix"), "-I", str(ROOT / "host")]
        layout_o = tmp / "game_layout.o"   # (no warning in it; the shim is built as the game builds it)
        subprocess.run([a.cc, *inc, "-Wall", "-Wextra", "-Werror", "-c", str(ROOT / "host" / "game_layout.c"), "-o", str(layout_o)],
                       check=True)
        subprocess.run([a.cc, *inc, str(layout_o), str(ROOT / "host" / "posix" / "win32_posix.c"), str(tmp / "main.c"),
                        "-o", str(built), "-lpthread", "-ldl"], check=True)

        def run(exe):
            return subprocess.run([str(exe)], cwd=tmp, check=True, capture_output=True, text=True, timeout=60).stdout

        # a game folder from before: moved, renamed, deleted, kept, as the docstring says; a second start changes nothing
        game = tmp / "a" / "game"
        exe = game_folder(game, built)
        check(game, run(exe))
        out = run(exe)
        assert "moved" not in out and "stayed" not in out, out
        check(game, "%d logs and shader caches moved" % (len(LOGS) + len(CACHES) + len(RENAMED)))

        # a file already in cache/ or logs/ is never written over: the loose one stays, and the log names it
        game = tmp / "b" / "game"
        exe = game_folder(game, built)
        (game / "cache").mkdir()
        (game / "cache" / "longshot_programs.bin").write_text("newer")
        (game / "logs").mkdir()
        (game / "logs" / "longshot_log.txt").write_text("newer log")
        out = run(exe)
        assert (game / "cache" / "longshot_programs.bin").read_text() == "newer"
        assert (game / "longshot_programs.bin").read_text() == "longshot_programs.bin"
        assert (game / "logs" / "longshot_log.txt").read_text() == "newer log"
        assert "2 files stayed where they were" in out, out

        # the game in bin/ beside image.bin: the folder above bin/ is the game folder, nothing goes into bin/
        game = tmp / "c" / "game"
        exe = game_folder(game, built, in_bin=True)
        check(game, run(exe))
        assert not (game / "bin" / "cache").exists() and not (game / "bin" / "logs").exists()
    print("PASS: cache/ and logs/ made; the logs and shader caches a build from before left loose moved into them (the Vulkan "
          "pipeline cache renamed longshot_vk_pipelines*, its cut-short writes and the old prompts deleted); the build's files, "
          "settings and saves stay; nothing written over; once only; the folder above bin/")


if __name__ == "__main__":
    main()
