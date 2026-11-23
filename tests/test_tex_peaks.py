"""Compile and run the rules for the renderer's texture peaks when the view changes (gpu/vk/tex_peaks.h; no game/GPU), and check
that vk_texture.cpp still applies them where they belong, each behind its setting."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]   # (tests/run_all.py reads it)

ROOT = Path(__file__).resolve().parents[1]

NEEDLES = (
    '#include "tex_peaks.h"',
    # (the made bytes, counted where total_bytes grows, the targets apart)
    "total_bytes += x.bytes + x.pad;\n    made_ring.add(frame_no, x.bytes + x.pad, as_target);",
    # (the settings, each off unless set: the shorter age and the cadence; the pool's hold 120 as before)
    'getenv("RX_VK_EVICT_BURST_UNUSED") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_EVICT_BURST_UNUSED"))) : 0;',
    'static const bool burst_every = getenv("RX_VK_EVICT_BURST_EVERY") && atoi(getenv("RX_VK_EVICT_BURST_EVERY")) > 0;',
    "const bool burst = (burst_unused || burst_every) && tex_peaks::burst(made_sampled, burst_min);",
    # (since lot 46 the gate runs #1255's dead targets' sweep before it returns, and the pool's hold reads upstream's
    # RX_VK_POOL_IDLE first; unset it is still 120 on the console. Since lot 63 keep_due, RX_VK_EVICT_KEEP_FREE_MB's
    # (tests/test_tex_keep.py), opens the gate too)
    "if (!keep_due && !tex_peaks::pass_now(frame_no, pressed, burst, burst_every)) {",
    'if (const char* e = getenv("RX_VK_TARGET_POOL_HOLD")) return (uint64_t)std::max(4, atoi(e));',
    "#ifdef __PROSPERO__\n        return (uint64_t)120;",
    "if (!tex_peaks::pool_drop(pool_bytes, cap, frame_no, v[i].since, hold)) {",
    'getenv("RX_VK_TARGET_POOL_TRIM") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_TARGET_POOL_TRIM"))) : 16;',
    "n < tex_peaks::trim_most(pool_pressed, pool_bytes > cap, pool_trim_most());",
    # (the age, per texture, the pressure's own rule left alone)
    "const uint64_t need = tex_peaks::need_unused(need_unused, burst_unused, burst && !pressed, !sweep, tex->gpu_written,\n"
    "                                                     frame_no - tex->made_frame, burst_made ? burst_made : need_unused);",
    'getenv("RX_VK_EVICT_BURST_MADE") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_EVICT_BURST_MADE"))) : 0;',
    "if (frame_no - last < need && !shifted) continue;",
    # (the line that says what is held)
    "vk: textures in a burst (%llu MB sampled and %llu MB of render targets made over the last 60 presents, present ",
)


def check_source():
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in NEEDLES:
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)
    frame = src[src.index("void texture_frame(uint64_t completed) {"):]
    # (the shorter age comes after the 8-present line and before every other protection: in-flight work, the rules for what the
    # GPU drew, the finalized ones kept; it only moves a sampled texture's own wait)
    # (since the Longshot merge the pass chooses first and destroys after: its 7/8 line counts the bytes chosen, pending_bytes)
    loop = frame[frame.index("for (auto& [last, tex] : order) {\n        // (total_bytes can have drifted below the textures' sum"):]
    i_need = loop.index("const uint64_t need = tex_peaks::need_unused(")
    for later in ("if (tex->last_value > completed) continue;",
                  "if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && frame_no - last < keep_rendered) continue;",
                  "if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && baked) continue;"):
        if loop.index(later) < i_need:
            raise SystemExit("vk_texture.cpp: a protection comes before the burst's age: " + later)
    # (the same pass still lets go in the pass that chose them, RX_VK_EVICT_FREE_NOW, after the burst line's inputs)
    if frame.index("if (!keep_due && !tex_peaks::pass_now(frame_no, pressed, burst, burst_every)) {") > frame.index("const uint64_t budget_unpressed = budget;"):
        raise SystemExit("vk_texture.cpp: the cadence's return is after the budget")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="tex-peaks-") as tmp:
        exe = Path(tmp) / "tex-peaks"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_tex_peaks.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
