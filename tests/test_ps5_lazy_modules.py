"""(ps5, lot 87) Lazy shader modules (gpu/vk/vk_progcache.cpp, after Longshot Oberon by rimidalv333, GPL-3.0): the stages read
from the program cache keep their SPIR-V zstd-packed and a stand-in for a module; the pipeline functions make a real module of it
for the call and destroy it after. The block is cut out of vk_progcache.cpp and run against stand-ins for the driver (a small
Vulkan of its own: no headers, no GPU, no game), on the console's defaults (__PROSPERO__) and the PC's:

- off on the PC unless RX_VK_LAZY_MODULES=1, on on the console unless RX_VK_LAZY_MODULES=0; off, nothing is wrapped;
- a stand-in has the low bit set, no driver module is made for it, and its packing is smaller than the SPIR-V; a program the driver
  faulted on gets none (as make_module);
- vkCreateGraphicsPipelines and vkCreateComputePipelines (the vkx pointers): each stand-in a real module made from the unpacked
  SPIR-V (the very words) for the call, the caller's own modules passed as they are, the caller's create infos untouched, every
  module made for the call destroyed after it, and no copy at all when no stage has a stand-in;
- a module that cannot be made: VK_ERROR_OUT_OF_HOST_MEMORY, no pipeline, the ones made for the call destroyed;
- vkDestroyShaderModule leaves stand-ins alone and destroys real modules;
- a stand-in let go (lazy_retire): its packing freed after the grace (RX_VK_DROP_PACKED), kept with RX_VK_DROP_PACKED=0; the
  report's line; no leak (the address sanitizer's, when the compiler has it);
- by reading: the cache's read packs when lazy modules are on, vk_main.cpp installs the wrappers right after load_device, the
  memory heaps' report says what they hold, the loader lets a loaded program's modules go only when every noted pipeline was
  built (program_cache_drop_loaded retires a stand-in before it lets it go), the switches are read in functions (never before
  main), and the packing lives in Stage, not in Program (whose size is part of the translator's stamp).

    python -I -B tests/test_ps5_lazy_modules.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import between, block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"windows": "links the system's libzstd: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
START = "// ---- (ps5, lot 87) Lazy shader modules"
END = "// ---- (ps5, lot 87) end of the lazy shader modules ----"

PRELUDE = r'''
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <zstd.h>
#include "longshot_env.h"
// (a Vulkan of the test's own: the names and members the block uses)
typedef struct VkDevice_T* VkDevice;
typedef struct VkShaderModule_T* VkShaderModule;
typedef struct VkPipelineCache_T* VkPipelineCache;
typedef struct VkPipeline_T* VkPipeline;
typedef int VkResult;
typedef int VkStructureType;
enum { VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1, VK_ERROR_INITIALIZATION_FAILED = -3 };
enum { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16 };
#define VK_NULL_HANDLE nullptr
#define VKAPI_ATTR
#define VKAPI_CALL
struct VkAllocationCallbacks;
struct VkShaderModuleCreateInfo { VkStructureType sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode; };
struct VkPipelineShaderStageCreateInfo { VkStructureType sType; const void* pNext; uint32_t flags; uint32_t stage; VkShaderModule module;
                                         const char* pName; const void* pSpecializationInfo; };
struct VkGraphicsPipelineCreateInfo { VkStructureType sType; const void* pNext; uint32_t flags; uint32_t stageCount;
                                      const VkPipelineShaderStageCreateInfo* pStages; };
struct VkComputePipelineCreateInfo { VkStructureType sType; const void* pNext; uint32_t flags; VkPipelineShaderStageCreateInfo stage; };
typedef VkResult (*PFN_vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
typedef void (*PFN_vkDestroyShaderModule)(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
typedef VkResult (*PFN_vkCreateGraphicsPipelines)(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*,
                                                  const VkAllocationCallbacks*, VkPipeline*);
typedef VkResult (*PFN_vkCreateComputePipelines)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*,
                                                 const VkAllocationCallbacks*, VkPipeline*);
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace mkwvk {
struct Dev { VkDevice device = (VkDevice)0x1000; } dev;
namespace vkx {
PFN_vkCreateShaderModule vkCreateShaderModule;
PFN_vkDestroyShaderModule vkDestroyShaderModule;
PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
PFN_vkCreateComputePipelines vkCreateComputePipelines;
}  // namespace vkx
std::string logged;
void logf(const char* fmt, ...) {
    char line[2048];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    logged += line, logged += "\n";
    if (std::getenv("TEST_VERBOSE")) std::fprintf(stderr, "%s\n", line);
}
struct PackedSpirv;
struct Stage {
    VkShaderModule module = VK_NULL_HANDLE;
    std::shared_ptr<const PackedSpirv> packed;
};
bool drop_loaded_modules_on() { return true; }
int64_t program_cache_remade() { return 0; }
}  // namespace mkwvk
'''

DRIVER = r'''
namespace mkwvk {
bool driver_faulted_program(uint64_t key) { return key == 0xbad; }
}
extern "C" int hos_ps5_memory_free(uint64_t* direct_mb, uint64_t* flexible_mb, uint64_t* largest_mb) {   // (the console's)
    *direct_mb = 1234, *flexible_mb = 60, *largest_mb = 1000;
    return 1;
}
// ---- the driver's stand-ins: a module is a heap copy of its words; what each call was given is recorded ----
struct Module { std::vector<uint32_t> code; };
static int made = 0, destroyed = 0, fail_create = 0, graphics_calls = 0, compute_calls = 0;
static std::vector<std::vector<uint32_t>> seen_code;   // (the words of each module a pipeline call was given)
static std::vector<VkShaderModule> seen_modules;
static const void* seen_ci = nullptr;
static VkResult d_create_module(VkDevice, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks*, VkShaderModule* m) {
    CHECK(ci->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && ci->codeSize % 4 == 0);
    if (fail_create) return VK_ERROR_INITIALIZATION_FAILED;
    auto* mod = new Module{std::vector<uint32_t>(ci->pCode, ci->pCode + ci->codeSize / 4)};
    CHECK(((uintptr_t)mod & 1) == 0);
    *m = (VkShaderModule)mod;
    made++;
    return VK_SUCCESS;
}
static void d_destroy_module(VkDevice, VkShaderModule m, const VkAllocationCallbacks*) {
    CHECK(m && ((uintptr_t)m & 1) == 0);   // (never a stand-in)
    delete (Module*)m;
    destroyed++;
}
static void see(VkShaderModule m) {
    CHECK(m && ((uintptr_t)m & 1) == 0);   // (the driver is never given a stand-in)
    seen_modules.push_back(m);
    seen_code.push_back(((Module*)m)->code);
}
static VkResult d_graphics(VkDevice, VkPipelineCache, uint32_t n, const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks*,
                           VkPipeline* out) {
    graphics_calls++, seen_ci = ci;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t k = 0; k < ci[i].stageCount; k++) see(ci[i].pStages[k].module);
        out[i] = (VkPipeline)(uintptr_t)(0x5000 + i);
    }
    return VK_SUCCESS;
}
static VkResult d_compute(VkDevice, VkPipelineCache, uint32_t n, const VkComputePipelineCreateInfo* ci, const VkAllocationCallbacks*,
                          VkPipeline* out) {
    compute_calls++, seen_ci = ci;
    for (uint32_t i = 0; i < n; i++) see(ci[i].stage.module), out[i] = (VkPipeline)(uintptr_t)(0x6000 + i);
    return VK_SUCCESS;
}
static std::vector<uint32_t> spirv_of(uint32_t seed, size_t words) {   // (SPIR-V-like: a magic, then repetitive words that pack well)
    std::vector<uint32_t> w(words);
    w[0] = 0x07230203;
    for (size_t i = 1; i < words; i++) w[i] = (uint32_t)((i % 37) * 0x10001u + seed * 7 + (i / 512));
    return w;
}
static VkShaderModule real_module(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4, ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    CHECK(d_create_module(nullptr, &ci, nullptr, &m) == VK_SUCCESS);
    return m;
}

int main() {
    using namespace mkwvk;
    vkx::vkCreateShaderModule = d_create_module, vkx::vkDestroyShaderModule = d_destroy_module;
    vkx::vkCreateGraphicsPipelines = d_graphics, vkx::vkCreateComputePipelines = d_compute;
    const bool want = std::atoi(std::getenv("TEST_WANT_ON"));
    CHECK(lazy_modules_on() == want);
    lazy_modules_install();
    if (!want) {   // (off: nothing wrapped, said once)
        CHECK(vkx::vkCreateGraphicsPipelines == d_graphics && vkx::vkCreateComputePipelines == d_compute &&
              vkx::vkDestroyShaderModule == d_destroy_module);
        CHECK(logged.find("lazy shader modules: off (RX_VK_LAZY_MODULES=0)") != std::string::npos);
        lazy_modules_report();
        CHECK(logged.find("stages read packed") == std::string::npos);
        std::puts("off: nothing wrapped");
        return 0;
    }
    CHECK(vkx::vkCreateGraphicsPipelines != d_graphics && vkx::vkCreateComputePipelines != d_compute &&
          vkx::vkDestroyShaderModule != d_destroy_module);
    CHECK(logged.find("lazy shader modules (Longshot Oberon's)") != std::string::npos);
    lazy_modules_install();   // (twice: once)
    // ---- the stand-in ----
    const std::vector<uint32_t> vs = spirv_of(1, 30000), fs = spirv_of(2, 20000), cs = spirv_of(3, 8000);
    Stage sv, sf, sc, bad;
    sv.module = lazy_pack(sv, vs, 0x1234);
    sf.module = lazy_pack(sf, fs, 0x1234);
    sc.module = lazy_pack(sc, cs, 0x77);
    CHECK(sv.module && ((uintptr_t)sv.module & 1) && sv.packed && lazy_tagged(sv.module) && lazy_of(sv.module) == sv.packed.get());
    CHECK(sv.packed->words == vs.size() && sv.packed->z.size() < vs.size() * 4 / 4 && made == 0);   // (no driver module; packed small)
    CHECK(!lazy_pack(bad, vs, 0xbad) && !bad.packed);   // (a program the driver faulted on: none, as make_module)
    // ---- a graphics pipeline: a stand-in and a module of the caller's ----
    const VkShaderModule own = real_module(fs);
    const int made0 = made;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].module = sv.module, stages[1].module = own;
    VkGraphicsPipelineCreateInfo gi{};
    gi.stageCount = 2, gi.pStages = stages;
    VkPipeline pl = VK_NULL_HANDLE;
    seen_code.clear(), seen_modules.clear();
    CHECK(vkx::vkCreateGraphicsPipelines(dev.device, nullptr, 1, &gi, nullptr, &pl) == VK_SUCCESS && pl);
    CHECK(graphics_calls == 1 && seen_code.size() == 2 && seen_code[0] == vs && seen_modules[1] == own);   // (the very words)
    CHECK(made == made0 + 1 && destroyed == 1);   // (one made for the call, destroyed after it; the caller's kept)
    CHECK(stages[0].module == sv.module && gi.pStages == stages && seen_ci != &gi);   // (the caller's untouched: a copy went down)
    // ---- no stand-in: the call as it is ----
    stages[0].module = own;
    CHECK(vkx::vkCreateGraphicsPipelines(dev.device, nullptr, 1, &gi, nullptr, &pl) == VK_SUCCESS && seen_ci == &gi);
    CHECK(made == made0 + 1 && destroyed == 1);
    // ---- two pipelines in one call, both stand-ins ----
    VkPipelineShaderStageCreateInfo two[2]{};
    two[0].module = sv.module, two[1].module = sf.module;
    VkGraphicsPipelineCreateInfo gis[2]{};
    gis[0].stageCount = 1, gis[0].pStages = &two[0], gis[1].stageCount = 1, gis[1].pStages = &two[1];
    VkPipeline pls[2]{};
    seen_code.clear(), seen_modules.clear();
    CHECK(vkx::vkCreateGraphicsPipelines(dev.device, nullptr, 2, gis, nullptr, pls) == VK_SUCCESS && pls[0] && pls[1]);
    CHECK(seen_code.size() == 2 && seen_code[0] == vs && seen_code[1] == fs && made == made0 + 3 && destroyed == 3);
    // ---- a compute pipeline ----
    VkComputePipelineCreateInfo ci{};
    ci.stage.module = sc.module;
    seen_code.clear();
    CHECK(vkx::vkCreateComputePipelines(dev.device, nullptr, 1, &ci, nullptr, &pl) == VK_SUCCESS && compute_calls == 1);
    CHECK(seen_code.size() == 1 && seen_code[0] == cs && made == made0 + 4 && destroyed == 4 && ci.stage.module == sc.module);
    // ---- a module that cannot be made: no pipeline, the call's modules destroyed ----
    fail_create = 1;
    pls[0] = pls[1] = (VkPipeline)1;
    CHECK(vkx::vkCreateGraphicsPipelines(dev.device, nullptr, 2, gis, nullptr, pls) == VK_ERROR_OUT_OF_HOST_MEMORY && !pls[0] && !pls[1]);
    CHECK(vkx::vkCreateComputePipelines(dev.device, nullptr, 1, &ci, nullptr, &pl) == VK_ERROR_OUT_OF_HOST_MEMORY && !pl);
    CHECK(graphics_calls == 3 && compute_calls == 1 && made == made0 + 4 && destroyed == 4);
    fail_create = 0;
    // ---- vkDestroyShaderModule: a stand-in left alone, a real module destroyed ----
    vkx::vkDestroyShaderModule(dev.device, sv.module, nullptr);
    CHECK(destroyed == 4 && sv.packed);
    vkx::vkDestroyShaderModule(dev.device, own, nullptr);
    CHECK(destroyed == 5);
    // ---- a stand-in let go: its packing freed after the grace, or kept with RX_VK_DROP_PACKED=0 ----
    const bool drop = std::atoi(std::getenv("TEST_WANT_DROP"));
    CHECK(drop_packed_on() == drop);
    std::weak_ptr<const PackedSpirv> watch = sv.packed;
    lazy_retire(sv);
    vkx::vkDestroyShaderModule(dev.device, sv.module, nullptr);
    sv.module = VK_NULL_HANDLE;
    CHECK(destroyed == 5);
    if (drop) {
        CHECK(!sv.packed && !watch.expired());   // (in the grave: a build that took the stand-in may still be making its module)
        lazy_modules_report();
        CHECK(!watch.expired() && logged.find("0 packings freed (0 KB)") != std::string::npos);
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        lazy_modules_report();
        CHECK(watch.expired() && logged.find("1 packings freed (") != std::string::npos);
    } else {
        CHECK(sv.packed && !watch.expired());
        lazy_modules_report();
        CHECK(logged.find("0 packings freed (0 KB) (RX_VK_DROP_PACKED=0)") != std::string::npos);
    }
    CHECK(logged.find("3 stages read packed, ") != std::string::npos && logged.find("4 modules made for builds (2 not made)") !=
          std::string::npos && logged.find(std::string("free direct memory ") + std::getenv("TEST_WANT_FREE") + " MiB") != std::string::npos);
    std::printf("on: %d modules made for builds and destroyed after them; the packing %s\n", made - made0,
                drop ? "freed after the grace" : "kept (RX_VK_DROP_PACKED=0)");
    return 0;
}
'''


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def check_sources():
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    shader_h = (ROOT / "gpu/vk/vk_shader.h").read_text(encoding="utf-8")
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    loader = (ROOT / "gpu/vk/vk_programs.cpp").read_text(encoding="utf-8")
    lazy = between(cache, START, END)
    need("Longshot Oberon by rimidalv333" in lazy, "the block does not say where it comes from")
    # (the cache's read: a stand-in when lazy modules are on, the driver's module otherwise)
    need("st.module = lazy_modules_on() ? lazy_pack(st, spirv, key) : make_module(spirv, key, nullptr);" in
         block(cache, "void program_cache_read("), "program_cache_read does not pack its stages")
    # (the wrappers right after the device's functions are loaded, before any pipeline is built)
    i = main_cpp.index("load_device(dev.device);")
    need(main_cpp.index("lazy_modules_install();", i) - i < 80, "lazy_modules_install is not right after load_device")
    heaps = tex.index("        vma_log_heaps();\n        vma_log_image_blocks();\n")
    need("        lazy_modules_report();" in tex[heaps:heaps + 300], "the heaps' report does not say the packings")
    # (the loader: modules let go only when every noted pipeline of the variant was built; a stand-in retired before it goes)
    work = loader[loader.index("size_t built = 0, left = 0;"):]
    work = work[:work.index("census.load_programs++;")]
    need("if (built && !left && drop_loaded_modules_on()) program_cache_drop_loaded(*p);" in work and "left++; continue;" in work and
         "census.load_pipes_cold++, left++;" in work, "the loader lets modules go with a noted pipeline not built")
    drop = block(cache, "size_t program_cache_drop_loaded(Program& p) {")
    need(drop.index("lazy_retire(st)") < drop.index("vkDestroyShaderModule(dev.device, st.module, nullptr);"),
         "program_cache_drop_loaded does not retire a stand-in before it lets it go")
    need("if (lazy_tagged(st.module)) lazy_retire(st);   // (lot 87)" in block(cache, "size_t program_cache_drop_modules() {"),
         "program_cache_drop_modules does not retire stand-ins")
    # (the switches read in functions, at their first use after main: never at namespace scope, before main)
    for name in ("RX_VK_LAZY_MODULES", "RX_VK_DROP_PACKED", "RX_VK_DROP_LOADED_MODULES"):
        for m in re.finditer(name, cache):
            line = cache[cache.rfind("\n", 0, m.start()) + 1:cache.find("\n", m.start())]
            if "getenv" in line:
                need(line.startswith("        "), "%s is read outside a function: %s" % (name, line.strip()))
    need(not re.search(r"^(static )?(const )?bool \w+ = (lazy_modules_on|drop_packed_on|drop_loaded_modules_on)\(\)", cache, re.M),
         "a switch is kept at namespace scope (read before main)")
    # (the packing in Stage, not in Program: Program's size is part of the translator's stamp, Stage's is not)
    stage = block(shader_h, "struct Stage {")
    program = block(shader_h, "struct Program {")
    need("std::shared_ptr<const PackedSpirv> packed;" in stage and "PackedSpirv" not in program, "the packing is not Stage's alone")
    need("vk_progcache.cpp" not in open(ROOT / "nx2/translator_files.py", encoding="utf-8").read().split("FILES = ")[1][:200],
         "vk_progcache.cpp is in the translator's stamp")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    check_sources()
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    lazy = between(cache, START, END)
    need("constexpr std::chrono::seconds kPackedGrace{10};" in lazy, "the grace is no longer 10 s")
    lazy = lazy.replace("constexpr std::chrono::seconds kPackedGrace{10};", "constexpr std::chrono::seconds kPackedGrace{1};")   # (1 s here)
    src = PRELUDE + "namespace mkwvk {\n" + lazy + "\n}  // namespace mkwvk\n" + DRIVER
    with tempfile.TemporaryDirectory(prefix="ps5-lazy-") as tmp:
        t = Path(tmp)
        (t / "lazy.cpp").write_text(src, encoding="utf-8")
        (t / "probe.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                 "-Wno-missing-field-initializers", "-I", str(ROOT / "gpu/vk/compat"), "-pthread"]
        sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if subprocess.run([a.cxx] + sanitize + [str(t / "probe.cpp"), "-o", str(t / "probe")], capture_output=True).returncode:
            sanitize = []
            print("(no address sanitizer here: the leak check is not run)")
        for name, defines in (("pc", []), ("console", ["-D__PROSPERO__=1"])):
            r = subprocess.run([a.cxx] + flags + sanitize + defines + [str(t / "lazy.cpp"), "-o", str(t / name), "-lzstd"],
                               capture_output=True, text=True)
            if r.returncode:
                sys.stderr.write(r.stderr[-4000:])
                raise SystemExit("FAIL: the block did not compile (%s)" % name)
        clean = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
        clean.update(ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        cases = [("pc", {}, 0, 1), ("pc", {"RX_VK_LAZY_MODULES": "1"}, 1, 1), ("console", {}, 1, 1),
                 ("console", {"RX_VK_LAZY_MODULES": "0"}, 0, 1), ("console", {"RX_VK_DROP_PACKED": "0"}, 1, 0),
                 ("console", {"RX_VK_LAZY_MODULES": "off"}, 0, 1)]
        for exe, env, on, drop in cases:
            e = dict(clean, TEST_WANT_ON=str(on), TEST_WANT_DROP=str(drop), TEST_WANT_FREE="1234" if exe == "console" else "-1", **env)
            r = subprocess.run([str(t / exe)], env=e, capture_output=True, text=True, timeout=120)
            if r.returncode:
                sys.stderr.write(r.stdout + r.stderr[-4000:])
                raise SystemExit("FAIL: %s with %s" % (exe, env or "no switch"))
            print("%-8s %-28s %s" % (exe, " ".join("%s=%s" % kv for kv in env.items()) or "(defaults)", r.stdout.strip()))
    print("PASS: lazy shader modules: stand-ins with the low bit, a real module made for each build and destroyed after it, the "
          "caller's modules and create infos as they were, stand-ins never destroyed, packings freed after the grace; the console's "
          "defaults and the switches")


if __name__ == "__main__":
    main()
