"""Compile and run the rules that hold down the renderer's memory while a scene loads (gpu/vk/load_peak.h; no game/GPU),
and check that vk_texture.cpp still applies them where they belong."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]   # (tests/run_all.py reads it)

ROOT = Path(__file__).resolve().parents[1]

# (what vk_texture.cpp must still hold: the rule used by the present's reaping and by the pass's own, behind its switch)
NEEDLES = (
    '#include "load_peak.h"',
    "load_peak::reapable(it->value, completed, frame_no, it->at, hold)",
    "reap_dead(completed, hold);",
    'getenv("RX_VK_EVICT_FREE_NOW")',
    "if (free_now && n) {",
    "reap_dead(completed, hold, &freed)",
    "if (tex->last_value > completed) continue;",
    "load_peak::arena_free(a.value, rec_value, done, a.size, n)",
    'getenv("RX_VK_STAGING_RECHECK")',
    "done = gpu_completed_now();",
    'getenv("RX_VK_ZERO_DEPTH_MB")',
    "if (!dirty && upload_depth_untouched(tex, total)) return true;",
    "u.first = !tex.uploaded, u.drawn = tex.gpu_written, u.sparse = tex.sparse;",
    "u.untouched = guest_untouched(p, total);",
    "u.shadowed = u.untouched && memory_has_shadow(tex.gpu, total);",
    # (the invariant the depth rule stands on: whatever the GPU writes into a texture marks it uploaded, and create() clears a
    # new depth image to 0.0)
    "tex->gpu_written = true;\n    tex->gpu_gen = memory_generation();\n    tex->released = false;\n    tex->uploaded = true;",
    "const VkClearDepthStencilValue z{0.0f, 0};\n        gt_note_tex(gt::CLEAR_NEW_DS, x);",
)


def check_source():
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in NEEDLES:
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "uint64_t gpu_completed_now() {\n    return completed();\n}" not in replay:
        raise SystemExit("vk_replay.cpp no longer reads the timeline for the staging (gpu_completed_now)")
    # (the immediate reaping comes after the pass has let its textures go, never before the eviction loop)
    if src.index("reap_dead(completed, hold, &freed)") < src.index("destroy_later(*tex);\n        forget_gpu(tex);"):
        raise SystemExit("vk_texture.cpp: the pass's own reaping is before its eviction")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="load-peak-") as tmp:
        exe = Path(tmp) / "load-peak"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_load_peak.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
