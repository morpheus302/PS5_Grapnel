"""(ps5, lot 63) Sampled textures kept while the console's free direct memory is large, behind RX_VK_EVICT_KEEP_FREE_MB
(gpu/vk/tex_peaks.h: KeepRule, KeepPass, keep_due; wired in vk_texture.cpp's texture_frame).

Checked here without the game or a GPU (tests/test_tex_keep.cpp, compiled and run):

- KeepRule::room: unset (0) or with the free memory not known, nothing held; on once the free memory as the pass would leave
  it is the threshold plus the hysteresis, then what stays over the threshold, at most the cap; off under the threshold, and
  off until the hysteresis is passed again;
- KeepPass: only the sampled textures the pass chose, unused under RX_VK_EVICT_KEEP_UNUSED, are candidates; the most recently
  used stay first, while they fit;
- keep_due: the pass runs at a present between its tenths only while textures are held and the free memory is more than
  64 MiB under the threshold;
- a pass reduced to what decides a texture's fate (the loop as vk_texture.cpp has it, then the rule), on 300 random caches:
  unset, or under the threshold, what goes is texture for texture what went before; far over it, no sampled texture under
  RX_VK_EVICT_KEEP_UNUSED goes; the render targets go as before in every case, and nothing goes that did not; what stays is
  the most recently used; the free memory once the pass let go stays at the threshold or over; never more than the cap;
- passes in a row with the camera going round 12 views: today's rules make each view again at every turn, the rule makes
  each texture once while the memory allows, and nothing changes when it does not.

And in vk_texture.cpp: the settings and their defaults (off unless RX_VK_EVICT_KEEP_FREE_MB is set), the free memory read
on the console only, the gate that adds keep_due to tex_peaks::pass_now, the rule's block after the pass chose (its loop
untouched) and inside "if (keep_free_mb)", the only place that takes anything out of what the pass chose.

What this cannot show: the console's free memory and how fast it moves, the GPU, the game's real textures, the effect on the
frame times (dist/next/63-garder-textures/LISEZMOI.md: to be measured there).

    python -I -B tests/test_tex_keep.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block_at(text, start):
    """The text from start to the brace that closes the first one opened in it"""
    i = text.index(start)
    j = text.index("{", i)
    depth = 0
    for k in range(j, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if not depth:
                return text[i:k + 1]
    raise SystemExit("unbalanced braces after: " + start)


def at(text, part, what):
    i = text.find(part)
    if i < 0:
        raise SystemExit("vk_texture.cpp has no " + what + " (RX_VK_EVICT_KEEP_FREE_MB): " + part.strip()[:100])
    return i


def check_source():
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    frame = block_at(src, "void texture_frame(uint64_t completed) {")
    fails = []

    def need(cond, what):
        if not cond:
            fails.append(what)

    need('#include "tex_peaks.h"' in src, "vk_texture.cpp includes tex_peaks.h")
    # (the settings: off unless RX_VK_EVICT_KEEP_FREE_MB is set; 512 MB, 1800 presents, 256 MiB otherwise)
    for line in ('getenv("RX_VK_EVICT_KEEP_FREE_MB") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_EVICT_KEEP_FREE_MB"))) : 0;',
                 '(uint64_t)(getenv("RX_VK_EVICT_KEEP_MB") ? std::max(0, atoi(getenv("RX_VK_EVICT_KEEP_MB"))) : 512) << 20;',
                 'getenv("RX_VK_EVICT_KEEP_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_EVICT_KEEP_UNUSED"))) : 1800;',
                 'getenv("RX_VK_EVICT_KEEP_HYST_MB") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_EVICT_KEEP_HYST_MB"))) : 256;'):
        need(frame.count(line) == 1, "the setting, read once with its default: " + line)
    # (the free memory: the console's measure, only with the setting; -1 elsewhere, which keeps the rule off)
    read = ("    int64_t keep_free_now = -1;      // (MiB; -1: not known, the rule off)\n#ifdef __PROSPERO__\n    if (keep_free_mb) {\n"
            "        const int64_t bytes = __atomic_load_n(&hos_ps5_free_estimate, __ATOMIC_RELAXED);\n"
            "        keep_free_now = bytes == -1 ? -1 : bytes < 0 ? 0 : bytes >> 20;\n    }\n#endif\n")
    need(read in frame, "the free memory read from hos_ps5_free_estimate under __PROSPERO__ only, with the setting only")
    need(frame.count("keep_free_now = ") == 2, "keep_free_now is set in one place (and declared -1)")
    # (the gate: tex_peaks::pass_now as before, or keep_due)
    gate = "    const bool keep_due = tex_peaks::keep_due(keep_held, keep_free_now, keep_free_mb);\n" \
           "    if (!keep_due && !tex_peaks::pass_now(frame_no, pressed, burst, burst_every)) {"
    need(gate in frame, "the gate: keep_due or tex_peaks::pass_now")
    # (the rule's block: after the loop that chooses, before what the choices go through; all of it under the setting)
    loop_start = at(frame, "    for (auto& [last, tex] : order) {\n        // (total_bytes can have drifted below the textures' sum",
                    "loop that chooses")
    loop = block_at(frame[loop_start:], "for (auto& [last, tex] : order) {")
    for name in ("keep_free_mb", "keep_rule", "keep_list", "keep_bytes", "keep_held", "keep_due", "keep_cap", "keep_unused"):
        need(name not in loop, "the loop that chooses is untouched by the rule: " + name)
    rule_at = at(frame, "    if (keep_free_mb) {\n        const bool can = !pressed && !compact_on();", "keep rule's block")
    rule = block_at(frame[rule_at:], "if (keep_free_mb) {")
    need(loop_start + len(loop) < rule_at, "the rule comes after the loop that chooses")
    need(rule_at < at(frame, "for (const Evict& e : evict) {   // (and each one's bookkeeping", "booking of the choices"),
         "the rule comes before the choices are booked and the images destroyed")
    need(rule_at < at(frame, "if (e.flush) ts.push_back(e.tex);", "write-backs"), "the rule comes before the write-backs")
    for part in ("for (const Evict& e : evict) look.chosen(e.tex, frame_no, keep_unused);",
                 "keep_rule.room(can ? keep_free_now : -1, look.let_go, keep_free_mb, keep_hyst, keep_cap);",
                 "keep_bytes = look.pick(room, keep_list);", "keep_held = keep_bytes;", "evict.erase("):
        need(part in rule, "the rule's block: " + part)
    need(frame.count("evict.erase(") == 1, "nothing else takes out of what the pass chose")
    need(frame.count("keep_held = ") == 2 and "static uint64_t keep_held = 0;" in frame, "keep_held is set by the rule alone")
    # (the burst line: unchanged without the setting)
    need('char keep_note[96] = "";' in frame and "        if (keep_free_mb)\n            snprintf(keep_note," in frame,
         "the burst line's addition only with the setting")
    if fails:
        raise SystemExit("vk_texture.cpp:\n  " + "\n  ".join(fails))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="tex-keep-") as tmp:
        exe = Path(tmp) / "tex-keep"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_tex_keep.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
