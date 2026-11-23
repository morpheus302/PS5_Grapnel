"""(ps5) The storage report: what the textures' storage holds of the game's memory, counted and logged, nothing changed.

A texture exists twice in the PS5 port: the game's copy in the guest window (often ASTC at full size) and the image the
renderer made of it. Every 600 presents, with the textures' line, the renderer logs how much of the game's memory is
texture storage, how much of that the window backs now, and for how much of the backed part the image already holds all of
it with nothing written since its upload ("vk: texture storage: ...", gpu/vk/vk_texture.cpp storage_report). It is a
measuring instrument for a decision (stop backing such storage, or not), so these checks hold it to that:
- its rules (gpu/vk/tex_storage.h: kinds, states, shared storage counted once, what a decommit would give back) are
  compiled and run against a window of 64 KiB units;
- the report reads the cache, the write generations and the window's bits and nothing else: it uploads nothing, reads no
  storage, and calls nothing that leaves a mark (texture_dirty renews a virtual pool's page map, gpu_wrote_into notes what
  it saw);
- its two lines are compiled with their format checked against their arguments (logf's own declaration checks nothing),
  fit logf's 1024 bytes with every figure at 99,999 MB, and read as expected for a small case;
- it runs with the textures' line only (every 600 presents), under RX_VK_STORAGE_REPORT, read at first use; off the
  console the window answers 0 and the line is off unless asked for;
- the window's count (host/posix/ps5_window.c hos_ps5_guest_backed_bytes) takes no lock, backs and maps nothing, and where
  PS5_PayloadSDK's model of the console's kernel is there it is built over it and run (scripts/ps5/build-plat-probe.sh
  host-test).

python -I -B tests/test_tex_storage.py [--cxx g++] [--cc cc] [--no-host-model]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEXTURE = ROOT / "gpu" / "vk" / "vk_texture.cpp"
TEXTURE_H = ROOT / "gpu" / "vk" / "vk_texture.h"
REPLAY = ROOT / "gpu" / "vk" / "vk_replay.cpp"
RULES = ROOT / "gpu" / "vk" / "tex_storage.h"
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


def code_of(text):
    """The text without its // comments (a comment may name what the code must not call)"""
    return "\n".join(line.split("//")[0] for line in text.split("\n"))


def check_rules(cxx):
    with tempfile.TemporaryDirectory(prefix="tex-storage-") as tmp:
        exe = Path(tmp) / "tex-storage"
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_tex_storage.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: gpu/vk/tex_storage.h: kinds, states, shared storage once and what a decommit would give back", flush=True)


def check_lines(tex, cxx):
    """The report's two lines, as vk_texture.cpp writes them, compiled with their format checked (logf itself is not) and
    run through a line buffer the size of logf's: every figure at 99,999 MB still fits, and a small case reads right"""
    report = block(tex, "void storage_report()")
    tail = report[report.index("    const auto mb = "):report.rindex("}")]
    need(tail.count("logf(") == 2, "the report's lines are not the last thing it does")
    fixture = r'''#include "%s"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
namespace ts = mkwvk::tex_storage;
std::vector<std::string> lines;
bool cut = false;
__attribute__((format(printf, 1, 2))) void logf(const char* fmt, ...) {
    char b[1024];   // (gpu/vk/vk_main.cpp logf's)
    std::va_list a;
    va_start(a, fmt);
    cut = cut || std::vsnprintf(b, sizeof b, fmt, a) >= (int)sizeof b;
    va_end(a);
    lines.push_back(b);
}
struct Game {
    uint64_t bytes = 0, backed = 0, pinned = 0;
};
void report(const Game& game, const ts::Totals& t, const uint64_t* state_once, const uint64_t* sum, const uint64_t* image, uint64_t clean_image,
            uint64_t in_virtual, uint64_t unmapped, size_t n, size_t n_clean, uint64_t left_out, uint64_t left_out_backed,
            uint64_t left_out_free) {
    const auto t0 = std::chrono::steady_clock::now();
%s
}
int main() {
    const uint64_t MB = 1ull << 20, big = 99999 * MB;
    Game game{big, big, big};
    ts::Totals t;
    uint64_t state_once[ts::STATES], sum[ts::KINDS], image[ts::KINDS];
    for (int k = 0; k < ts::KINDS; k++) t.once[k] = t.backed[k] = t.pinned[k] = t.clean_backed[k] = sum[k] = image[k] = big / 4;
    for (int s = 0; s < ts::STATES; s++) t.state_backed[s] = state_once[s] = big;
    for (int a = 0; a < ts::AGES; a++) t.clean_age[a] = t.free_from[a] = big;
    t.free_unread = big;
    report(game, t, state_once, sum, image, big, big, big, 99999, 99999, big, big, big);
    if (cut || lines.size() != 2) return std::fprintf(stderr, "FAIL: a line of the report does not fit logf's 1024 bytes\n"), 1;
    std::printf("the longest lines: %%zu and %%zu characters\n", lines[0].size(), lines[1].size());
    lines.clear();
    // a small case: each figure in its place
    Game g2{2134 * MB, 1876 * MB, 412 * MB};
    ts::Totals u;
    const uint64_t once[] = {402, 96, 121, 486}, backed[] = {398, 95, 117, 78}, pinned[] = {120, 0, 60, 32}, img[] = {229, 60, 142, 1002},
                   cb[] = {390, 92, 89, 0}, sb[] = {78, 571, 17, 22}, so[] = {486, 580, 17, 22}, ages[] = {402, 121, 40, 8},
                   from[] = {512, 148, 41, 8}, sums[] = {420, 100, 130, 560};
    uint64_t so2[ts::STATES], sum2[ts::KINDS], img2[ts::KINDS];
    for (int k = 0; k < ts::KINDS; k++)
        u.once[k] = once[k] * MB, u.backed[k] = backed[k] * MB, u.pinned[k] = pinned[k] * MB, u.clean_backed[k] = cb[k] * MB, sum2[k] = sums[k] * MB,
        img2[k] = img[k] * MB;
    for (int s = 0; s < ts::STATES; s++) u.state_backed[s] = sb[s] * MB, so2[s] = so[s] * MB;
    for (int a = 0; a < ts::AGES; a++) u.clean_age[a] = ages[a] * MB, u.free_from[a] = from[a] * MB;
    u.free_unread = 41 * MB;
    report(g2, u, so2, sum2, img2, 371 * MB, 64 * MB, 3 * MB, 731, 512, 310 * MB, 301 * MB, 288 * MB);
    for (const std::string& l : lines) std::printf("%%s\n", l.c_str());
    const char* want0 = "vk: texture storage: the game's textures take 2134 MB of its memory (virtual pools' apart), 1876 MB of it backed in the window "
                        "(412 MB pinned by imports); the 731 with an image: 1210 MB (ASTC 420, block 100, plain 130, targets 560), shared storage "
                        "once 1105 MB (ASTC 402, block 96, plain 121, targets 486; 64 MB through virtual pools, 3 MB more unmapped), backed 688 MB "
                        "(ASTC 398, block 95, plain 117, targets 78; 212 MB pinned), their images 1433 MB "
                        "(ASTC 229, block 60, plain 142, targets 1002); levels the images leave out: 310 MB of storage, 301 MB backed, 288 MB a "
                        "decommit would give back; counted in ";
    const char* want1 = "vk:   by what the image has of it (MB of storage, MB backed): uploaded and not written since 580, 571 (backed: ASTC 390, block "
                        "92, plain 89; last used in the last 8 presents 402, 600 121, 3600 40, before 8; 512 textures, their images 371 MB); written "
                        "since 22, 22; not uploaded yet 17, 17; nothing of it read 486, 78; a decommit would give back 41 MB of that last, and with "
                        "the uploaded and unwritten 512 MB (148 without those used in the last 8 presents, 41 the last 600, 8 the last 3600)";
    if (lines.size() != 2 || std::strncmp(lines[0].c_str(), want0, std::strlen(want0)) || lines[1] != want1)
        return std::fprintf(stderr, "FAIL: the report's lines do not read as the test expects\n"), 1;
    if (lines[0].find("off the console nothing says what is backed (0)") == std::string::npos)
        return std::fprintf(stderr, "FAIL: off the console the line does not say that the backed figures are not known\n"), 1;
}
''' % (RULES.as_posix(), tail)
    with tempfile.TemporaryDirectory(prefix="tex-storage-lines-") as tmp:
        cpp = Path(tmp) / "lines.cpp"
        exe = Path(tmp) / "lines"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wformat=2", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: gpu/vk/vk_texture.cpp: the report's two lines match their arguments, fit logf's buffer and read as expected", flush=True)


def check_renderer(tex, tex_h, replay):
    need('#include "tex_storage.h"' in tex, "vk_texture.cpp no longer includes the report's rules")
    report = block(tex, "void storage_report()")
    code = code_of(report)
    need(code.index("if (!storage_report_on()) return;") < code.index("textures"), "the switch is not asked before anything is walked")
    # the rules are tex_storage.h's, given what texture_touch itself goes by
    for part in ("ts::kind_of(x.astc, is_bc(x.format) || is_astc(x.format), gpu_image, x.mem_uploads != 0)",
                 "ts::state_of(x.uploaded, gpu_image, x.mem_uploads != 0, written)",
                 "const bool gpu_image = x.gpu_written && !x.only_cleared;",
                 "const bool looked_at = x.uploaded && !(gpu_image && !x.released);",
                 "std::max(x.upload_gen, x.only_cleared && !x.released ? x.gpu_gen : x.release_gen);",
                 "memory_written_since(guest, bytes, since)", "ts::pieces_of(spans, pieces);", "ts::count(pieces, window_backed, t);",
                 "texture_storage_runs(", "frame_no - std::min(frame_no, x.last_frame)"):
        need(part in code, "the report no longer has: " + part)
    touch = block(tex, "Texture* texture_touch(Texture* tex, bool for_sampling)")
    need("const uint32_t since = tex->gpu_written && tex->released ? std::max(tex->upload_gen, tex->release_gen) : tex->upload_gen;" in touch and
         "if (tex->gpu_written && !tex->released) {" in touch and
         "if ((!take && !tex->only_cleared) || tex->checked_epoch == epoch) return tex;" in touch and
         "if (!texture_dirty(*tex, std::max(tex->upload_gen, tex->gpu_gen), cpu)) return tex;" in touch,
         "texture_touch no longer asks what was written from the generation the report takes it to ask from")
    # the levels an image leaves out: where upload_astc_gpu starts to stage each layer, and upload goes by the image's levels
    astc = block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty)")
    need("const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);" in astc and
         "const uint64_t tail0 = level_geo(tex, 0).offset;" in astc and "const uint64_t at = (uint64_t)layers[k] * layer_size + tail0;" in astc,
         "upload_astc_gpu no longer stages each layer from its first kept level: the report's left-out levels go by that")
    need("const uint32_t gl = level + tex.skip;" in block(tex, "LevelGeo level_geo(const Texture& tex, uint32_t level)"),
         "level_geo no longer takes the image's level for the storage's level + skip")
    need("const uint64_t layer_size = x.layers > 1 ? x.L.layer_size : std::max<uint64_t>(x.L.layer_size, x.guest_size);" in code and
         "const uint64_t kept_from = level_geo(x, 0).offset;" in code and "if (x.skip && x.guest && x.gpu < (1ull << 40)) {" in code,
         "the levels an image leaves out are not counted from each layer's start to its first kept level")
    # a virtual pool's texture: its pages as they are mapped now, asked of the host, never through texture_dirty
    need("host->gpu_guest(x.gpu + off)" in code and "0x10000 - ((x.gpu + off) & 0xFFFF)" in code, "a virtual pool's pages are not walked by 64 KB page")
    # it measures: nothing uploaded, nothing read from the storage, nothing marked, nothing kept from one report to the next
    for never in ("texture_dirty(", "gpu_wrote_into(", "upload(", "upload_zeros(", "guest_bytes(", "gpu_read(", "gpu_ptr(", "gpu_write(",
                  "memory_stamp", "memory_written_runs(", "memory_note_texture_upload(", "memory_has_shadow(", "gpu_buffer(", "texture_touch",
                  "destroy_later(", "watch_tex(", "memcpy(", "vkCmd", "->uploaded =", ".uploaded =", "upload_gen =", "last_frame =",
                  "mem_uploads++", "hos_ps5_guest_untouched", "decommit("):
        need(never not in code, "the report does more than count: " + never)
    need("const Texture& x = *kv.second;" in code and "for (const auto& kv : textures)" in code, "the report does not walk the textures as const")
    need(code.count("logf(") == 2 and '"vk: texture storage: the game\'s textures take %llu MB of its memory (virtual pools\' apart)' in report and
         '"vk:   by what the image has of it (MB of storage, MB backed): uploaded and not written since %llu, %llu' in report,
         "the report's two lines changed (the notes and the tools that read the log go by them)")

    # where it runs: with the textures' line, every 600 presents, and nowhere else
    frame = block(tex, "void texture_frame(uint64_t completed)")
    periodic = block(frame, "if (frame_no % 600 == 0) {\n        vma_log_heaps();")
    need("storage_report();" in periodic and periodic.index('logf("vk: textures: decoded ASTC') < periodic.index("storage_report();"),
         "the report is not made with the textures' line, every 600 presents")
    need(len(re.findall(r"\bstorage_report\(\);", tex)) == 1, "the report is made from somewhere else too")

    # the switch: read at first use, on by default on the console only
    on = block(tex, "bool storage_report_on()")
    need(tex.count('getenv("RX_VK_STORAGE_REPORT")') == 1 and 'getenv("RX_VK_STORAGE_REPORT")' in on and "static const bool on = [] {" in on,
         "RX_VK_STORAGE_REPORT is not read inside storage_report_on alone, at its first call (the console's settings are read in main)")
    need(re.search(r"#ifdef __PROSPERO__\s+return !\(e && e\[0\] == '0'\);\s+#else\s+return e && e\[0\] == '1';\s+#endif", on),
         "the report is not on by default on the console and off elsewhere")

    # the window's answer: the console's, 0 anywhere else
    backed = block(tex, "uint64_t window_backed(uint64_t lo, uint64_t hi, uint64_t* pinned, uint64_t* free)")
    need(re.search(r"#ifdef __PROSPERO__\s+return hos_ps5_guest_backed_bytes\(host->guest_base\(\) \+ lo, \(size_t\)\(hi - lo\), pinned, free\);\s+"
                   r"#else.*if \(pinned\) \*pinned = 0;\s+if \(free\) \*free = 0;\s+return 0;\s+#endif", backed, re.S),
         "off the console the window does not answer 0")
    need(console_only(tex, "hos_ps5_guest_backed_bytes("), "hos_ps5_guest_backed_bytes is named outside #ifdef __PROSPERO__")

    # "uploaded from memory": counted where an upload starts to read, not where it is answered with a clear
    upload = block(tex, "bool upload_inner(Texture& tex, const Ranges* dirty) {")
    zeros = block(tex, "bool upload_zeros(Texture& tex, uint64_t total)")
    need(upload.index("if (!dirty && upload_zeros(tex, total)) return true;") < upload.index("tex.mem_uploads++;") <
         upload.index("upload_astc_gpu(tex, total, dirty)"), "an upload is not counted after the clear was tried and before anything is read")
    need(tex.count("mem_uploads++") == 1 and "mem_uploads" not in zeros, "mem_uploads is counted elsewhere, or by an upload answered with a clear")
    need("uint32_t mem_uploads = 0;" in tex_h, "vk_texture.h: Texture has no mem_uploads")
    for line in code_of(tex).split("\n"):   # (the count decides nothing: read by the report alone)
        if "mem_uploads" in line and "mem_uploads++" not in line:
            need("x.mem_uploads != 0" in line and ("ts::kind_of(" in line or "ts::state_of(" in line), "mem_uploads is read outside the report: " + line.strip())
    need("only_cleared =" not in code and "gpu_gen =" not in code and "release_gen =" not in code, "the report sets what texture_touch goes by")

    # every texture the game has: the host's page counts, read and not changed
    need("void texture_storage_runs(void (*run)(uint64_t lo, uint64_t hi, void* ctx), void* ctx);" in tex_h,
         "vk_texture.h does not declare texture_storage_runs")
    runs = block(replay, "void texture_storage_runs(void (*run)(uint64_t lo, uint64_t hi, void* ctx), void* ctx)")
    runs_code = code_of(runs)
    for never in ("fetch_add", "fetch_sub", ".store(", "make_unique", "tex_pages_add("):
        need(never not in runs_code, "texture_storage_runs changes the page counts: " + never)
    need("std::lock_guard lk(tex_pages_mu);" in runs_code and "static_assert(sizeof(std::atomic<uint16_t>) == sizeof(uint16_t)" in runs_code,
         "the chunks are not taken under the lock they are made under, or the counts' size is not checked")
    need("void texture_created_of(const nvn_tex* t) { tex_pages_add(t, +1); }" in replay and "tex_pages_add(t, -1);" in replay,
         "the page counts no longer follow the game's textures as they are made and finalized")
    print("PASS: gpu/vk/vk_texture.cpp: the storage report counts and logs with the textures' line, and changes nothing", flush=True)


def check_window(window, header, host_test):
    need("uint64_t hos_ps5_guest_backed_bytes(const void *p, size_t n, uint64_t *pinned_bytes, uint64_t *free_bytes);" in header,
         "host/posix/ps5.h does not declare hos_ps5_guest_backed_bytes")
    fn = block(window, "uint64_t hos_ps5_guest_backed_bytes(const void *p, size_t n, uint64_t *pinned_bytes, uint64_t *free_bytes)")
    need("if (!n || !hos_ps5_in_guest(p, n)) return 0;" in fn, "no range, or one outside the window, is not answered 0")
    need(fn.index("if (pinned_bytes) *pinned_bytes = 0;") < fn.index("return 0;") and fn.index("if (free_bytes) *free_bytes = 0;") < fn.index("return 0;"),
         "a range refused leaves what was asked for unset")
    # (it may run while a thread faults, and from the renderer every report: no lock, no kernel call, nothing written)
    for never in ("lock_gib", "back_unit", "unback_unit", "sceKernel", "set_bit", "set_zero_bit", "memset", "__atomic_store", "__atomic_fetch",
                  "volatile", "zero_bit"):
        need(never not in fn, "counting locks, maps, changes or reads something: " + never)
    need("else if (part == UNIT) whole += UNIT;" in fn and "__builtin_popcount(b & ~k)" in fn,
         "what a decommit would give back is not the units whole inside the range, backed and not pinned")
    decommit = block(window, "int hos_ps5_guest_decommit(void *p, size_t n)")
    need("if (ulo >= a && uhi <= end && bit(u) && pin_bit(u)) {" in decommit and "} else if (ulo >= a && uhi <= end) r = unback_unit(u);" in decommit,
         "hos_ps5_guest_decommit's rule changed: the count's free_bytes is defined by it")
    need("backed_tests();" in host_test and "hos_ps5_guest_backed_bytes(G(lo), (size_t)(hi - lo), &pinned, &freeable)" in host_test,
         "scripts/ps5/plat-probe/host_window_test.c no longer runs the backed-bytes checks")
    print("PASS: host/posix/ps5_window.c: the count takes no lock and changes nothing; a decommit's rule is its own", flush=True)


def check_host_model(cc):
    stack = Path(os.environ.get("PS5_STACK") or ROOT / ".." / ".." / "ps5-stack")
    model = stack / "PS5_PayloadSDK" / "platform" / "tests" / "host_kernel.c"
    if os.name == "nt" or not model.is_file() or not shutil.which("bash") or not shutil.which(cc):
        print("skipped: the window's code over the console's kernel model (no PS5_PayloadSDK platform, or no bash or %s here)" % cc, flush=True)
        return
    # (built in a folder of its own: tests/test_zero_upload.py builds and runs the same test, and tests/run_all.py runs both at once)
    with tempfile.TemporaryDirectory(prefix="tex-storage-window-") as tmp:
        r = subprocess.run(["bash", str(ROOT / "scripts" / "ps5" / "build-plat-probe.sh"), "host-test"], cwd=ROOT,
                           env=dict(os.environ, CC=cc, PS5_STACK=str(stack), PLAT_PROBE_OUT=tmp), capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=300)
    out = r.stdout + r.stderr
    wanted = ("PASS 64 MiB nothing touched: no byte backed, and asking backed nothing",
              "PASS three units committed of eight: their bytes, all of them a decommit's to give back",
              "PASS a unit partly inside the range counts for its part, and only one whole inside it is a decommit's to give back",
              "PASS a unit read through the shared zeros is not counted",
              "PASS pinned units are counted as backed and as pinned, not as what a decommit gives back",
              "PASS 80 units across a GiB's end, 46 backed and 5 of them pinned: 2944 KiB backed, 320 pinned, 2624 a decommit's",
              "PASS a decommit of the range gave back those 41 units; the pinned ones stay, nothing more to give",
              "PASS no range, and a range not all inside the window: 0 backed")
    missing = [w for w in wanted if w not in out]
    if r.returncode or missing or "PASSED: 0 failed" not in out or "FAIL " in out:
        print(out[-6000:])
        raise SystemExit("FAIL: the window's host test (exit %d)%s" % (r.returncode, "; not passed: " + "; ".join(missing) if missing else ""))
    print("PASS: host/posix/ps5_window.c over the console's kernel model: %d checks, the backed-bytes ones among them" % out.count("\nPASS "), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--cc", default="cc")
    p.add_argument("--no-host-model", action="store_true")
    a = p.parse_args()
    check_rules(a.cxx)
    tex = TEXTURE.read_text(encoding="utf-8")
    check_lines(tex, a.cxx)
    check_renderer(TEXTURE.read_text(encoding="utf-8"), TEXTURE_H.read_text(encoding="utf-8"), REPLAY.read_text(encoding="utf-8"))
    check_window(WINDOW.read_text(encoding="utf-8"), HEADER.read_text(encoding="utf-8"), HOST_TEST.read_text(encoding="utf-8"))
    if not a.no_host_model:
        check_host_model(a.cc)


if __name__ == "__main__":
    main()
