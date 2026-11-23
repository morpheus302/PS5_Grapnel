"""(ps5) A texture whose storage nothing ever wrote is cleared, not uploaded.

The game samples render targets before it draws them (its 12288 x 12288 D16 shadow map: 288 MB), and their first use
uploaded them from guest memory nothing had written: on the console a staging buffer of the whole 288 MiB, direct memory
from the moment it is made, for zeros, at a scene change, where the title is closest to its memory floor. The console's
guest window knows which of its units ever got memory of their own (host/posix/ps5_window.c hos_ps5_guest_untouched), and
upload (gpu/vk/vk_texture.cpp) clears the image when none of the storage's has.

These checks keep the rules that make that the same picture as the upload it replaces:
- only a whole upload, only formats where zero bytes are zero texels, only storage the window answers for and no kept copy
  of an earlier submit covers, and nothing recorded before all of that is known;
- the texture is still stamped with the memory generation read before the upload and still joins the submit-time capture,
  which is what uploads it again once the game writes its storage;
- the window looks at its units under the lock a unit is given its memory under;
- RX_VK_ZERO_UPLOAD is read at first use, and off the console the answer is always no.
The format rule is compiled and run. Where PS5_PayloadSDK's model of the console's kernel is there (hookshot-ps5's
ps5-stack, or PS5_STACK), the window's code is built over it and run too (scripts/ps5/build-plat-probe.sh host-test).

python -I -B tests/test_zero_upload.py [--cxx g++] [--no-host-model]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cxx", "g++"]   # (tests/run_all.py reads it)
TEXTURE = ROOT / "gpu" / "vk" / "vk_texture.cpp"
WINDOW = ROOT / "host" / "posix" / "ps5_window.c"
HEADER = ROOT / "host" / "posix" / "ps5.h"
HOST_TEST = ROOT / "scripts" / "ps5" / "plat-probe" / "host_window_test.c"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def console_only(text, name):
    """Every use of name lies between an #ifdef __PROSPERO__ and its #else or #endif"""
    inside, depth, uses = [], 0, 0
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
            if "__PROSPERO__" in s and "!" not in s and "||" not in s:
                inside.append(depth)
        elif s.startswith("#else") or s.startswith("#elif"):
            if inside and inside[-1] == depth:
                inside.pop()
        elif s.startswith("#endif"):
            if inside and inside[-1] == depth:
                inside.pop()
            depth -= 1
        elif name in line and not s.startswith("//"):
            uses += 1
            if not inside:
                return False
    return uses > 0


def check_renderer(tex):
    upload = block(tex, "bool upload_inner(Texture& tex, const Ranges* dirty) {")
    call = "if (!dirty && upload_zeros(tex, total)) return true;"
    need(call in upload, "upload does not try upload_zeros for a whole upload (and only for one)")
    at = upload.index(call)
    # (after what upload refuses for any texture; before anything is counted, staged, imported or read)
    for before in ("tex.samples != VK_SAMPLE_COUNT_1_BIT", "total > (1ull << 31)",
                   "tex.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)"):
        need(upload.index(before) < at, "upload_zeros is tried before upload's own refusal: " + before)
    for after in ("st_uploads++", "upload_astc_gpu(tex, total, dirty)", "upload_unswizzle_gpu(tex, total, dirty)", "staging(",
                  "guest_bytes(", "host->gpu_read("):
        need(at < upload.index(after), "upload_zeros is tried after " + after)

    zeros = block(tex, "bool upload_zeros(Texture& tex, uint64_t total)")
    need(zeros.index("if (!zero_upload_on()) return false;") < zeros.index("zero_upload_exact("), "the switch is not asked first")
    need("zero_upload_exact(tex.aspect, tex.sparse, tex.astc, tex.bc, is_bc(tex.format) || is_astc(tex.format), widen_from)"
         in zeros, "the format rule is not given the texture's aspect, sparse, ASTC, BC3, block format and widening")
    need("widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);" in zeros and
         "widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);" in upload,
         "the widening asked is not upload's own")
    need('if (tex.gpu >= (1ull << 40)) return refused("a virtual pool");' in zeros, "a texture of a virtual pool is not left to the upload")
    need("if (!p || avail < total) return refused(" in zeros, "storage that is not one run of host memory is not left to the upload")
    # (the levels and layers upload copies into, no other: its own loop and its own rule)
    for part in ("const uint32_t copy_layers = tex.type == VK_IMAGE_TYPE_3D ? 1 : tex.layers;",
                 "const uint64_t src_off = g.offset + (uint64_t)layer * tex.L.layer_size;", "if (src_off >= total) continue;"):
        need(part in zeros and part in upload, "upload_zeros and upload disagree on what is copied: " + part)
    need("(uint64_t)stride * (g.th - 1) + (uint64_t)g.tw * tex.bpb > total) return false;" in zeros,
         "a linear texture whose rows would be read past its storage is not left to the upload")
    # (nothing recorded, counted or logged before the window and the shadow have answered)
    ask = "if (memory_has_shadow(tex.gpu, total)) return refused("
    need("if (!guest_untouched(p, total)) return refused(" in zeros and ask in zeros and
         zeros.index("if (!guest_untouched(p, total)) return refused(") < zeros.index(ask),
         "the window and the kept copies of earlier submits are not both asked")
    for after in ("upload_cmd()", "vkCmdClearDepthStencilImage", "vkCmdClearColorImage", "++st_zero_uploads", "logf("):
        need(zeros.index(ask) < zeros.index(after), after + " comes before the window's answer")
    for never in ("staging(", "guest_bytes(", "gpu_read(", "gpu_buffer(", "memcpy("):
        need(never not in zeros, "upload_zeros stages, imports or reads the storage: " + never)
    need("const VkClearDepthStencilValue z{0.0f, 0};" in zeros and "const VkClearColorValue z{};" in zeros, "the clear is not to zero")
    need(zeros.count("VK_IMAGE_LAYOUT_GENERAL, &z, (uint32_t)ranges.size(), ranges.data());") == 2,
         "the clears are not of the ranges the upload would copy into, in the images' layout")
    clear = zeros.index("vkCmdClearColorImage")
    need("vkCmdPipelineBarrier2(cmd, &di);" in zeros[clear:] and "b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;" in zeros[clear:],
         "no barrier after the clear (a later copy into the image in the same command buffer could run before it)")
    need("vk: zero upload: #%u %#llx" in zeros and "KB neither staged nor copied (%llu textures, %llu MB so far)" in zeros,
         "the log line does not name the texture, its size and the running total")
    need(re.search(r"if \(n <= 40 \|\| n % 100 == 0 \|\|", zeros), "the log line is not limited")

    on = block(tex, "bool zero_upload_on()")
    # (merged v2.2.0: upstream's all-zero upload, #2175, answers to the same name, read at its first call too)
    need(tex.count('getenv("RX_VK_ZERO_UPLOAD")') == 3 and 'getenv("RX_VK_ZERO_UPLOAD")' in on and
         tex.count('getenv("RX_VK_ZERO_UPLOAD")') - on.count('getenv("RX_VK_ZERO_UPLOAD")') ==
         block(tex, "bool upload_all_zero(Texture& tex, uint64_t total, const Ranges* dirty, bool widened_format) {").count('getenv("RX_VK_ZERO_UPLOAD")'),
         "RX_VK_ZERO_UPLOAD is not read inside zero_upload_on alone (a static initializer runs before the console's settings are read)")
    need("return !(e && e[0] == '0');" in on, "RX_VK_ZERO_UPLOAD is not on unless it is 0")
    need(re.search(r"#ifdef __PROSPERO__.*static const bool on = \[\].*#else\s+return false;\s+#endif", on, re.S),
         "off the console zero_upload_on does not answer no")
    untouched = block(tex, "bool guest_untouched(const uint8_t* p, uint64_t n)")
    need(re.search(r"#ifdef __PROSPERO__\s+return hos_ps5_guest_untouched\(p, \(size_t\)n\) != 0;\s+#else.*return false;\s+#endif",
                   untouched, re.S), "off the console guest_untouched does not answer no")
    need(console_only(tex, "hos_ps5_guest_untouched("), "hos_ps5_guest_untouched is named outside #ifdef __PROSPERO__")

    # texture_touch: the upload's bookkeeping is the same for the clear, and it is what uploads the texture again
    touch = block(tex, "Texture* texture_touch(Texture* tex, bool for_sampling)")
    up = touch.index("const bool up_ok = upload(*tex, partial || sparse_written ? &dirty : nullptr);")
    need(touch.rindex("const uint32_t gen = memory_generation();", 0, up) > 0, "the memory generation is not read before the upload")
    done = touch[up:]
    ok = block(done, "if (up_ok) {")   # (that branch alone: the other one stamps a failed upload)
    for part in ("memory_note_texture_upload(tex->gpu, tex->guest_size);", "tex->uploaded = true;", "tex->upload_gen = gen;"):
        need(part in ok, "an upload that succeeded no longer does: " + part)
    need("const bool partial = !no_partial && !gpu_dirty && tex->uploaded && !tex->gpu_written && !tex->released;" in touch and
         "if (tex->uploaded && !gpu_dirty && !texture_dirty(*tex, since, dirty)) return tex;" in touch,
         "a texture uploaded before is no longer checked against the write generations and uploaded in part")
    print("PASS: gpu/vk/vk_texture.cpp: a whole upload of storage nothing wrote is a clear, with the upload's bookkeeping", flush=True)


def check_format_rule(tex, cxx):
    fn = block(tex, "bool zero_upload_exact(")
    fixture = f'''#include <cstdio>
#include <cstdlib>
typedef unsigned VkImageAspectFlags;
enum {{ VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_IMAGE_ASPECT_DEPTH_BIT = 2, VK_IMAGE_ASPECT_STENCIL_BIT = 4 }};
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
int main() {{
    const VkImageAspectFlags c = VK_IMAGE_ASPECT_COLOR_BIT, d = VK_IMAGE_ASPECT_DEPTH_BIT, s = VK_IMAGE_ASPECT_STENCIL_BIT;
    CHECK(zero_upload_exact(c, false, false, false, false, 0));    // (RGBA8, R16F, R11G11B10F ...: the bytes are the texels)
    CHECK(zero_upload_exact(d, false, false, false, false, 0));    // (D16, D24, D32F: the shadow map)
    CHECK(!zero_upload_exact(s, false, false, false, false, 0));   // (stencil alone)
    CHECK(!zero_upload_exact(d | s, false, false, false, false, 0));
    CHECK(!zero_upload_exact(c, true, false, false, false, 0));    // (sparse)
    CHECK(!zero_upload_exact(c, false, true, false, false, 0));    // (ASTC decoded)
    CHECK(!zero_upload_exact(c, false, true, true, false, 0));     // (and kept as BC3)
    CHECK(!zero_upload_exact(c, false, false, true, false, 0));
    CHECK(!zero_upload_exact(c, false, false, false, true, 0));    // (BC1..7 and native ASTC blocks)
    CHECK(!zero_upload_exact(c, false, false, false, false, 3));   // (RGB8 widened: alpha 0xFF)
    CHECK(!zero_upload_exact(c, false, false, false, false, 6));
    CHECK(!zero_upload_exact(c, false, false, false, false, 12));
    CHECK(!zero_upload_exact(d, true, false, false, false, 0));
    std::printf("PASS: zeros are exact for plain colour and depth alone\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="zero-upload-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def check_window(window, header, host_test):
    need("int hos_ps5_guest_untouched(const void *p, size_t n);" in header, "host/posix/ps5.h does not declare hos_ps5_guest_untouched")
    fn = block(window, "int hos_ps5_guest_untouched(const void *p, size_t n)")
    need("if (!n || !hos_ps5_in_guest(p, n)) return 0;" in fn, "no range, or one outside the window, is not answered 0")
    need("hi = (a + n + UNIT - 1) >> UNIT_SHIFT" in fn, "the range's last unit is not looked at")
    lock, unlock = fn.index("lock_gib(g);"), fn.index("unlock_gib(g);")
    looks = [m.start() for m in re.finditer(r"backed\[u >> 3\]|bit\(u\)", fn)]
    need(looks and all(lock < at < unlock for at in looks), "a unit's bit is read outside its GiB's lock")
    for never in ("back_unit", "sceKernel", "zero_bit", "set_bit", "memset"):
        need(never not in fn, "asking changes or maps something, or takes the shared zeros for memory of its own: " + never)
    # (what the lock is worth: a unit is writable only between its mapping and its bit, both made under the lock)
    back = block(window, "static int32_t back_unit(uint64_t u)")
    need(back.index("sceKernelMapDirectMemory(&placed, UNIT, PROT_RW") < back.index("set_bit(u, 1);"), "back_unit sets the bit before it maps")
    # (hos_ps5_guest_commit backs under the lock itself since the reclaim of zero units: tests/test_zero_reclaim.py)
    for caller in ("static int32_t back_units(", "int hos_ps5_guest_commit(", "int hos_ps5_guest_pin("):
        body = block(window, caller)
        need(body.index("lock_gib(g);") < body.index("back_unit(u)") < body.index("unlock_gib(g);"), caller + " backs a unit outside the lock")
    # (2026-11-12: and back_units' branch for a cold unit, which brings its own bytes back: tests/test_cold.py)
    need(len(re.findall(r"\bback_unit\(u\)", window)) == 4, "a unit is backed from somewhere this test does not know")
    fault = block(window, "int hos_ps5_guest_fault2(const void *p, int write) {")
    need("PS5_KERNEL_PROT_CPU_READ, PS5_KERNEL_MAP_FIXED, zero_start" in fault and "set_bit(" not in fault,
         "a read fault's shared zeros are no longer read-only, or count as memory of the unit's own")
    need("untouched_tests();" in host_test and "hos_ps5_guest_untouched(G(ZERO_AT), 288 * MIB) == 1" in host_test,
         "scripts/ps5/plat-probe/host_window_test.c no longer runs the untouched checks")
    print("PASS: host/posix/ps5_window.c: untouched is asked under the lock a unit gets its memory under, and changes nothing", flush=True)


def check_host_model(cc):
    stack = Path(os.environ.get("PS5_STACK") or ROOT / ".." / ".." / "ps5-stack")
    model = stack / "PS5_PayloadSDK" / "platform" / "tests" / "host_kernel.c"
    if os.name == "nt" or not model.is_file() or not shutil.which("bash") or not shutil.which(cc):
        print("skipped: the window's code over the console's kernel model (no PS5_PayloadSDK platform, or no bash or %s here)" % cc, flush=True)
        return
    r = subprocess.run(["bash", str(ROOT / "scripts" / "ps5" / "build-plat-probe.sh"), "host-test"], cwd=ROOT,
                       env=dict(os.environ, CC=cc, PS5_STACK=str(stack)), capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=300)
    out = r.stdout + r.stderr
    wanted = ("PASS 288 MiB nothing touched: untouched, and asking backed nothing",
              "PASS 8 MiB of it read through the shared zeros (128 units, no memory of their own): still untouched",
              "PASS one byte written in it: not untouched",
              "PASS what lies before and after that block still is",
              "PASS a range is its units",
              "PASS given back, it is untouched again",
              "PASS a unit committed and never written answers 0",
              "PASS a unit an import pinned answers 0",
              "PASS a byte written past the GiB's end",
              "PASS no range, and a range not all inside the window: 0",
              "beside a thread writing and freeing the range: never untouched")
    missing = [w for w in wanted if w not in out]
    if r.returncode or missing or "PASSED: 0 failed" not in out or "FAIL " in out:
        print(out[-6000:])
        raise SystemExit("FAIL: the window's host test (exit %d)%s" % (r.returncode, "; not passed: " + "; ".join(missing) if missing else ""))
    print("PASS: host/posix/ps5_window.c over the console's kernel model: %d checks, the untouched ones among them" % out.count("\nPASS "), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--cc", default="cc")
    p.add_argument("--no-host-model", action="store_true")
    a = p.parse_args()
    tex = TEXTURE.read_text(encoding="utf-8")
    check_renderer(tex)
    check_format_rule(tex, a.cxx)
    check_window(WINDOW.read_text(encoding="utf-8"), HEADER.read_text(encoding="utf-8"), HOST_TEST.read_text(encoding="utf-8"))
    if not a.no_host_model:
        check_host_model(a.cc)


if __name__ == "__main__":
    main()
