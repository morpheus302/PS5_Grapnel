"""(ps5, lot 67) The render thread's three per-draw switches of gpu/vk/draw_lean.h, each off unless set: RX_VK_TEX_LEAN,
RX_VK_SMALL_COSTS (upstream Longshot #391's per-draw part) and RX_VK_INDEX_FIRST.

Checked without the game or Vulkan, on code cut from the live gpu/vk/vk_draw.cpp and compiled with tests/test_draw_lean.cpp:

- RX_VK_TEX_LEAN: build_descriptors' loop over a stage's sampled textures (cut whole, with texture_view_of cut whole) and the lean
  way (textures_lean and the branch that makes the writes, cut whole) run from the same mock world, 3,000 random stages: the
  registrations and slot versions, the eviction generation (a miss that evicts too), the epoch, handle_hits entries standing or
  stale, textures the GPU wrote or with no view, words that are a storage buffer's address, handles 0, vertex, fragment and compute
  stages, descriptors of several elements, stages past the prefetch window. Everything they leave must be the same: the image
  infos, the writes (bindings, counts, where they point), the GPU-written list, the descriptor reuse's notes, every touch and
  texture_view_of miss in order (with the upload deferral each one saw), the handle_hits entries, the dummies counted. And a
  handle table sends the stage to the loop (tex_lean_plain); in the source, the switch takes the lean way only with no per-texture
  diagnostic on (tex_lean_quiet) and leaves the loop as it was otherwise.
- RX_VK_SMALL_COSTS: dyn_same_fast against dyn_same (both cut) on 20,000 inputs and the edges (-0 and 0 the same, a NaN never,
  the last word's and byte's difference, a count of 0); the mask against % for every power-of-two alignment; in the source,
  uniform_range's two tests of the offset both through that one answer.
- RX_VK_INDEX_FIRST: first_index_for's index lands on the byte the old binding read, and it refuses an offset that is not a whole
  index or a firstIndex past 32 bits with the count after it; in the source, draw() binds at 0 only when it said so and passes its
  firstIndex to vkCmdDrawIndexed, else binds at the offset with firstIndex 0 as before.
- The switches' reading: RX_VK_TEX_LEAN unset/0/off, 1/on, check.

What this cannot show: the time saved on the console (dist/next/67-fil-de-rendu-par-dessin/LISEZMOI.md), the driver's own work, the
threads (the mock world is the render thread alone).

    python -I -B tests/test_draw_lean.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def braces_end(text, at):
    """The index just past the brace that closes the first '{' at or after at (strings, characters and // comments skipped)"""
    i = text.index("{", at)
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and text.startswith("//", i):
            i = text.index("\n", i)
            continue
        if c == '"' or c == "'":
            q = c
            i += 1
            while text[i] != q:
                i += 2 if text[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise SystemExit(f"no closing brace after offset {at}")


def cut(text, start, *, tail=""):
    """From start (which must be there exactly once) to its closing brace, and the tail written after it"""
    if text.count(start) != 1:
        raise SystemExit(f"expected one {start!r} in vk_draw.cpp, found {text.count(start)}")
    i = text.index(start)
    j = braces_end(text, i)
    if tail:
        if not text.startswith(tail, j):
            raise SystemExit(f"{start!r} does not end with {tail!r}")
        j += len(tail)
    return text[i:j] + "\n"


def need(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def check_source(draw):
    # (the build: the lean way only when no per-texture diagnostic is on and the stage is plain; the loop otherwise, unchanged)
    build = cut(draw, "void build_descriptors(const NvnState& s, const Program& p) {")
    need("const bool lean_quiet = lean_mode && tex_lean_quiet(p);" in build, "the lean way does not ask tex_lean_quiet")
    need("const bool lean_stage = lean_quiet && !info.texture_descriptors.empty() && tex_lean_plain(info);" in build,
         "the lean way does not ask tex_lean_plain")
    need("if (!(lean_stage && lean_mode == 1))\n        for (const auto& d : info.texture_descriptors) {\n            FINE_SCOPE(FT_TEX);" in build,
         "the loop does not run whenever the lean way is not taken")
    quiet = cut(draw, "bool tex_lean_quiet(const Program& p) {")
    for word in ("dump_on()", "watch_prog()", "log_img_wanted(p.key)", '"RX_VK_TRACE_TEX"', '"RX_VK_LOG_HITS"'):
        need(word in quiet, f"tex_lean_quiet does not ask {word}: the loop's per-texture diagnostic would be skipped")
    # (texture_view_of's key and test, which textures_lean reads through draw_lean.h)
    view = cut(draw, "VkImageView texture_view_of(uint32_t handle, VkImageViewType want, ViewUse use, VkFormat storage_as = VK_FORMAT_UNDEFINED,")
    need("const uint64_t key = (uint64_t)(handle & 0xFFFFF) | (uint64_t)want << 20 | (uint64_t)use << 28 | (uint64_t)storage_as << 32;" in view,
         "texture_view_of's key is no longer draw_lean::hit_key's")
    need("it->second.version == version && it->second.destroy_gen == dgen" in view,
         "texture_view_of's test of an entry is no longer draw_lean::hit_stands'")
    # (RX_VK_SMALL_COSTS in uniform_range and draw())
    uniform = cut(draw, "VkDescriptorBufferInfo uniform_range(const NvnState& s, const Stage& st, uint32_t index) {")
    need("const bool in_place = b && (mask ? draw_lean::aligned_pow2(b.offset, align) : b.offset % align == 0);" in uniform,
         "uniform_range's alignment test is not the switch's choice")
    need("after_gpu[gpu_written - 1][in_place ? 0 : 1]" in uniform and "if (in_place) return {b.buffer, b.offset, n};" in uniform,
         "uniform_range's two tests of the offset do not both go through in_place")
    need("b.offset % align" not in uniform.replace("b.offset % align == 0);", ""), "uniform_range tests the offset elsewhere with %")
    need("static const bool mask = draw_lean::small_costs_on() && draw_lean::is_pow2(align);" in uniform,
         "the mask is used for an alignment that is not a power of two")
    dyn = cut(draw, "void dynamic_state(VkCommandBuffer cmd, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, bool mesh) {")
    need("(draw_lean::small_costs_on() ? dyn_same_fast(df, kDynF, du, kDynU, db, nb, dyn_fast.fp, dyn_fast.up, dyn_fast.raw)\n"
         "                                     : dyn_same(df, kDynF, du, kDynU, db, nb, dyn_fast.fp, dyn_fast.up, dyn_fast.raw))" in dyn,
         "dynamic_state does not choose dyn_same_fast by the switch")
    drawfn = cut(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    # (RX_VK_INDEX_FIRST in draw())
    idx = drawfn[drawfn.index("    } else if (d.indexed) {"):]
    idx = idx[:idx.index("    } else {")]
    need("const bool at_start =\n"
         "                draw_lean::index_first_on() && draw_lean::first_index_for(ib.offset, unit, (uint32_t)d.count, &first_index);\n"
         "            if (!at_start) first_index = 0;\n"
         "            bind_index(cmd, ib.buffer, at_start ? 0 : ib.offset, itype);" in idx,
         "the index buffer is bound at 0 without first_index_for's answer, or at the offset with a firstIndex")
    need("vkCmdDrawIndexed(cmd, (uint32_t)d.count, (uint32_t)std::max(1, d.instances), first_index, d.base_vertex," in idx,
         "the draw does not pass its firstIndex")
    need(idx.count("vkCmdDrawIndexed(") == 1 and idx.count("bind_index(") == 1, "another index bind or draw in the indexed path")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    check_source(draw)
    cuts = {
        "handle_hit.inc": cut(draw, "struct HandleHit {", tail=";"),
        "view_type.inc": cut(draw, "VkImageViewType view_type(Shader::TextureType t) {"),
        "texture_view_of.inc": cut(draw, "VkImageView texture_view_of(uint32_t handle, VkImageViewType want, ViewUse use, VkFormat storage_as = VK_FORMAT_UNDEFINED,"),
        "textures_lean.inc": cut(draw, "void textures_lean(const NvnState& s, const Stage& st, std::vector<VkDescriptorImageInfo>& imgs,"),
        "tex_lean_plain.inc": cut(draw, "bool tex_lean_plain(const Shader::Info& info) {"),
        "write_lambda.inc": cut(draw, "    auto write = [&](VkDescriptorType type, uint32_t count) {\n        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};", tail=";"),
        "loop.inc": cut(draw, "        for (const auto& d : info.texture_descriptors) {\n            FINE_SCOPE(FT_TEX);"),
        "lean_branch.inc": cut(draw, "        if (lean_stage && lean_mode == 1) {"),
        "dyn_same.inc": cut(draw, "bool dyn_same(const float* fp, int nf, const uint32_t* up, int nu, const uint8_t* raw, int nb, const float* cfp,")
                        + cut(draw, "bool dyn_same_fast(const float* fp, int nf, const uint32_t* up, int nu, const uint8_t* raw, int nb, const float* cfp,"),
    }
    with tempfile.TemporaryDirectory(prefix="draw-lean-") as tmp:
        tmp = Path(tmp)
        for name, text in cuts.items():
            (tmp / name).write_text(text, encoding="utf-8")
        shutil.copy(ROOT / "gpu/vk/draw_lean.h", tmp / "draw_lean.h")
        shutil.copy(ROOT / "gpu/vk/flat_map.h", tmp / "flat_map.h")
        (tmp / "compat").mkdir()
        shutil.copy(ROOT / "gpu/vk/compat/longshot_env.h", tmp / "compat" / "longshot_env.h")
        exe = tmp / "draw-lean"
        build = subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                                "-Wno-unused-parameter", "-I", str(tmp), str(ROOT / "tests/test_draw_lean.cpp"), "-o", str(exe)],
                               capture_output=True, text=True)
        if build.returncode:
            sys.stderr.write(build.stdout + build.stderr)
            raise SystemExit("FAIL: the cuts did not compile")
        env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}   # (none of the caller's switches)
        run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        if run.returncode:
            raise SystemExit("FAIL: see above")
    print("PASS: test_draw_lean")


if __name__ == "__main__":
    main()
