// tests/test_draw_lean.cpp: tests/test_draw_lean.py compiles this with the pieces it cut from gpu/vk/vk_draw.cpp (*.inc, beside it)
// and runs it. A mock world for build_descriptors' sampled textures: the registrations (a slot's texture and version), the eviction
// generation and the epoch, the thread's handle_hits entries (standing or stale), textures the GPU wrote, textures with no view, a
// miss that evicts. The loop as build_descriptors has it (with texture_view_of, cut whole) and RX_VK_TEX_LEAN's way (textures_lean
// and the branch that makes the writes, cut whole) run from the same world, and everything they leave is compared.
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "draw_lean.h"
#include "flat_map.h"

// ---- the few Vulkan names the cuts use ----
typedef struct VkImageView_T* VkImageView;
typedef struct VkSampler_T* VkSampler;
#define VK_NULL_HANDLE nullptr
enum VkImageViewType {
    VK_IMAGE_VIEW_TYPE_1D = 0, VK_IMAGE_VIEW_TYPE_2D = 1, VK_IMAGE_VIEW_TYPE_3D = 2, VK_IMAGE_VIEW_TYPE_CUBE = 3,
    VK_IMAGE_VIEW_TYPE_1D_ARRAY = 4, VK_IMAGE_VIEW_TYPE_2D_ARRAY = 5, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY = 6
};
enum VkFormat { VK_FORMAT_UNDEFINED = 0, VK_FORMAT_R8G8B8A8_UNORM = 37, VK_FORMAT_D32_SFLOAT = 126 };
enum VkImageLayout { VK_IMAGE_LAYOUT_GENERAL = 1 };
enum VkDescriptorType { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1 };
enum VkStructureType { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35 };
enum VkShaderStageFlagBits { VK_SHADER_STAGE_VERTEX_BIT = 1, VK_SHADER_STAGE_FRAGMENT_BIT = 0x10, VK_SHADER_STAGE_COMPUTE_BIT = 0x20 };
struct VkDescriptorImageInfo {
    VkSampler sampler;
    VkImageView imageView;
    VkImageLayout imageLayout;
};
struct VkWriteDescriptorSet {
    VkStructureType sType;
    const void* pNext;
    void* dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    VkDescriptorType descriptorType;
    const VkDescriptorImageInfo* pImageInfo;
    const void* pBufferInfo;
    const void* pTexelBufferView;
};

namespace Shader {
enum class TextureType { Color1D, ColorArray1D, Color2D, ColorArray2D, Color3D, ColorCube, ColorArrayCube, Buffer, Color2DRect };
struct TextureDescriptor {
    TextureType type = TextureType::Color2D;
    bool is_depth = false;
    bool is_integer = false;
    uint32_t handle_program = ~0U;
    uint32_t count = 1;
    uint32_t size_shift = 3;
    uint32_t cbuf_index = 2;
    uint32_t cbuf_offset = 0;
    uint32_t pointer_mask = 0;   // (the mock's handle_is_storage_pointer: element k's word is a storage buffer's address)
};
struct Info {
    std::vector<TextureDescriptor> texture_descriptors;
};
}  // namespace Shader

// ---- the mock world ----
enum class ViewUse { Sample, Attachment, Storage };
struct nvn_tex_d { int target, format; uint32_t width, height, depth, levels; };
struct nvn_tex { nvn_tex_d d; uint64_t gpu; };
struct nvn_view { bool set_format, set_target; int format, target; };
struct Texture {
    uint32_t id = 0;
    bool gpu_written = false;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 4, height = 4, depth = 1, layers = 1, levels = 1;
    bool sparse = false;
    uint64_t gpu = 0;
    void* image = nullptr;
};
struct NvnState { std::vector<uint32_t> words; };
struct Stage {
    Shader::Info info;
    uint32_t vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    int nvn_stage = 1;
};
struct Program { uint64_t key = 0x1234; };
struct DescWriter {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imgs;
};
struct ParLock { ParLock() {} };
#define FINE_SCOPE(ph) (void)0
#define FINE_N(c, n) (void)0
void logf(const char*, ...) {}

struct World {
    std::vector<uint32_t> version;   // a slot's registration count
    std::vector<int> reg;            // a slot's texture, -1: none registered
    std::vector<Texture> textures;
    std::vector<uint8_t> quick_ok;   // texture_touch_quick's answer, by texture
    std::vector<uint8_t> no_view;    // texture_view makes nothing for it
    uint64_t dgen = 1;
    uint32_t epoch = 1;
    int evict_at_get = -1;           // the texture_get call that bumps the eviction generation (an eviction meanwhile)
    int gets = 0;
    std::vector<std::string> events;
};
World W;
bool upload_defer_ok = false;
int64_t st_dummy_textures = 0;
struct { VkImageView view[7]; } dummies = {{(VkImageView)0xD0, (VkImageView)0xD1, (VkImageView)0xD2, (VkImageView)0xD3,
                                           (VkImageView)0xD4, (VkImageView)0xD5, (VkImageView)0xD6}};
std::vector<const Texture*> g_sampled;
std::vector<const Texture*>& draw_sampled() { return g_sampled; }
bool desc_fp_on = false;
std::vector<uint32_t> g_fp;
void desc_fp_note(uint32_t handle) {
    if (!desc_fp_on) return;
    g_fp.push_back(handle);
}
void ev(const char* what, long a, long b) {
    char line[96];
    snprintf(line, sizeof line, "%s %ld %ld defer %d", what, a, b, (int)upload_defer_ok);
    W.events.push_back(line);
}

#include "handle_hit.inc"   // (struct HandleHit, cut)
FlatMap<uint64_t, HandleHit> g_hits;
FlatMap<uint64_t, HandleHit>& handle_hits() { return g_hits; }

uint32_t texture_slot_version(int id) { return id >= 0 && id < (int)W.version.size() ? W.version[id] : 0; }
uint64_t texture_destroy_generation() { return W.dgen; }
uint32_t texture_epoch() { return W.epoch; }
bool texture_touch_quick(Texture* t, bool sampling) {
    ev("quick", t->id, sampling);
    return W.quick_ok[t->id];
}
Texture* texture_touch(Texture* t, bool sampling) {
    ev("touch", t->id, sampling);
    return t;
}
bool texture_of(int id, nvn_tex* t, nvn_view* v, bool* has_view) {
    ev("of", id, 0);
    if (id < 0 || id >= (int)W.reg.size() || W.reg[id] < 0) return false;
    *t = nvn_tex{{1, 2, 4, 4, 1, 1}, (uint64_t)W.reg[id]};
    *v = nvn_view{false, false, 0, 0};
    *has_view = id & 1;
    return true;
}
Texture* texture_get(const nvn_tex* t, bool sampling) {
    ev("get", (long)t->gpu, sampling);
    if (W.gets++ == W.evict_at_get) W.dgen++;
    return t->gpu < W.textures.size() ? &W.textures[t->gpu] : nullptr;
}
VkImageView texture_view(Texture* tex, const nvn_tex*, const nvn_view*, VkImageViewType vt, ViewUse use, VkFormat* last,
                         VkFormat storage_as) {
    ev("view", tex->id, vt);
    *last = VK_FORMAT_R8G8B8A8_UNORM;
    (void)use, (void)storage_as;
    return W.no_view[tex->id] ? VK_NULL_HANDLE : (VkImageView)(uintptr_t)(0x10000 + tex->id * 16 + vt);
}
uint32_t view_level(const nvn_view*) { return 0; }
uint32_t image_level(const Texture*, uint32_t level) { return level; }
VkSampler sampler_for(int id, bool compare) { return (VkSampler)(uintptr_t)(0x1000 + id * 2 + (compare ? 1 : 0)); }
// (lot 70, RX_VK_RT_NO_ANISO: off here, as by default; the twin is its own sampler, so a stage that took it would differ)
VkSampler rt_no_aniso_sampler(bool no_aniso, const Texture* tex, int id, VkSampler smp, uint64_t prog) {
    (void)tex, (void)id, (void)prog;
    return no_aniso ? (VkSampler)(uintptr_t)(0x9000 + id) : smp;
}
bool is_depth_format(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT; }
uint32_t handle_of(const NvnState& s, const Stage&, const Shader::TextureDescriptor& d, uint32_t k) {
    return s.words[d.cbuf_offset / 4 + k];
}
bool handle_is_storage_pointer(const Shader::Info&, const Shader::TextureDescriptor& d, uint32_t k) { return d.pointer_mask >> k & 1; }
void stage_handles(const NvnState&, const Stage&, uint32_t, uint32_t n, uint32_t* out) {
    for (uint32_t i = 0; i < n; i++) out[i] = 0;
}
// (the loop's diagnostics, all off: the lean way is taken only then, tex_lean_quiet)
bool dump_on() { return false; }
void dump_input(char, uint32_t, uint32_t, const Texture*) {}
const Texture* trace_tex_hit = nullptr;
bool trace_tex_wanted(const Texture*) { return false; }
bool log_img_wanted(uint64_t) { return false; }
VkFormat last_view_format = VK_FORMAT_UNDEFINED;
uint64_t watch_prog() { return 0; }
void watch_note(const char*, ...) {}
bool dump_sampled_wanted(uint64_t) { return false; }
std::vector<Texture*> g_dump_sampled;
std::vector<Texture*>& dump_sampled() { return g_dump_sampled; }
int dump_seq() { return 0; }
int64_t st_lean_stages, st_lean_textures, st_lean_original;

#include "view_type.inc"         // (view_type, cut)
#include "texture_view_of.inc"   // (texture_view_of, cut whole)
#include "textures_lean.inc"     // (textures_lean, cut whole)
#include "tex_lean_plain.inc"    // (tex_lean_plain, cut)

/// build_descriptors' loop over a stage's sampled textures (cut whole), with its write lambda (cut)
void run_loop(const NvnState& s, const Stage& st, const Program& p, DescWriter& out, uint32_t& binding) {
    const Shader::Info& info = st.info;
    const bool no_aniso = false;   // (lot 70: RX_VK_RT_NO_ANISO off)
#include "write_lambda.inc"
#include "loop.inc"
    (void)p;
}
/// RX_VK_TEX_LEAN=1's branch in build_descriptors (cut whole), as a stage takes it
void run_lean(const NvnState& s, const Stage& st, const Program& p, DescWriter& out, uint32_t& binding) {
    const Shader::Info& info = st.info;
    const bool lean_stage = true;
    const int lean_mode = 1;
    const bool no_aniso = false;   // (lot 70: RX_VK_RT_NO_ANISO off)
#include "write_lambda.inc"
#include "lean_branch.inc"
    (void)p;
}

struct Result {
    std::vector<VkDescriptorImageInfo> imgs;
    std::vector<std::array<uint64_t, 4>> writes;   // binding, count, type, the first image info's index
    uint32_t binding = 0;
    std::vector<const Texture*> sampled;
    std::vector<uint32_t> fp;
    std::vector<std::string> events;
    int64_t dummies = 0;
    uint64_t dgen = 0;
    bool defer_left = false;
    std::vector<std::array<uint64_t, 7>> hits;   // every key the world can ask: view, tex, destroy_gen, level, version, epoch, sampling
};

struct Scenario {
    World world;
    FlatMap<uint64_t, HandleHit> hits;
    NvnState s;
    Stage st;
    bool fp_on = false;
    uint32_t binding0 = 0;
};

Result run(const Scenario& sc, bool lean) {
    W = sc.world;
    g_hits = sc.hits;
    g_sampled.clear(), g_fp.clear();
    desc_fp_on = sc.fp_on;
    upload_defer_ok = false;
    st_dummy_textures = 0;
    DescWriter out;
    size_t n = 0;
    for (const auto& d : sc.st.info.texture_descriptors) n += d.count;
    out.imgs.reserve(n);   // (build_descriptors reserves the program's count: stable pointers)
    uint32_t binding = sc.binding0;
    Program p;
    if (lean) run_lean(sc.s, sc.st, p, out, binding);
    else run_loop(sc.s, sc.st, p, out, binding);
    Result r;
    r.imgs = out.imgs;
    for (const auto& w : out.writes)
        r.writes.push_back({w.dstBinding, w.descriptorCount, (uint64_t)w.descriptorType, (uint64_t)(w.pImageInfo - out.imgs.data())});
    r.binding = binding;
    r.sampled = g_sampled;
    r.fp = g_fp;
    r.events = W.events;
    r.dummies = st_dummy_textures;
    r.dgen = W.dgen;
    r.defer_left = upload_defer_ok;
    for (uint32_t slot = 0; slot < W.version.size(); slot++)
        for (uint32_t vt = 0; vt < 7; vt++) {
            const uint64_t key = draw_lean::hit_key(slot, vt, (uint32_t)ViewUse::Sample, 0);
            if (const auto* e = g_hits.find(key)) {
                const HandleHit& h = e->second;
                r.hits.push_back({(uint64_t)(uintptr_t)h.view, (uint64_t)(uintptr_t)h.tex, h.destroy_gen, h.level, h.version, h.tex_epoch,
                                  (uint64_t)h.sampling});
            } else {
                r.hits.push_back({~0ull, 0, 0, 0, 0, 0, 0});
            }
        }
    return r;
}

bool same(const Result& a, const Result& b, std::string* why) {
    if (a.imgs.size() != b.imgs.size()) return *why = "image info count", false;
    for (size_t i = 0; i < a.imgs.size(); i++)
        if (a.imgs[i].sampler != b.imgs[i].sampler || a.imgs[i].imageView != b.imgs[i].imageView ||
            a.imgs[i].imageLayout != b.imgs[i].imageLayout)
            return *why = "image info " + std::to_string(i), false;
    if (a.writes != b.writes) return *why = "writes", false;
    if (a.binding != b.binding) return *why = "binding", false;
    if (a.sampled != b.sampled) return *why = "GPU-written list", false;
    if (a.fp != b.fp) return *why = "fingerprint notes", false;
    if (a.events != b.events) return *why = "touches and misses", false;
    if (a.dummies != b.dummies) return *why = "dummies counted", false;
    if (a.dgen != b.dgen) return *why = "eviction generation", false;
    if (a.defer_left != b.defer_left) return *why = "upload deferral left set", false;
    if (a.hits != b.hits) return *why = "handle_hits", false;
    return true;
}

Scenario make(std::mt19937& rng, int index) {
    auto pick = [&](int n) { return (int)(rng() % (uint32_t)n); };
    Scenario sc;
    World& w = sc.world;
    const int slots = 48, textures = 20;
    w.version.resize(slots), w.reg.resize(slots);
    for (int i = 0; i < slots; i++) w.version[i] = 1 + pick(3), w.reg[i] = pick(5) ? pick(textures) : -1;
    w.textures.resize(textures), w.quick_ok.resize(textures), w.no_view.resize(textures);
    for (int i = 0; i < textures; i++) {
        w.textures[i].id = i;
        w.textures[i].gpu_written = pick(3) == 0;
        w.quick_ok[i] = pick(2);
        w.no_view[i] = pick(8) == 0;
    }
    w.dgen = 5 + pick(2);
    w.epoch = 7 + pick(2);
    w.evict_at_get = pick(3) ? -1 : pick(4);
    // (entries the thread resolved before: standing, or stale by the slot's version or the eviction generation; touched this
    // submission or not; some without a texture or a view)
    auto some_slot = [&] { return (uint32_t)(pick(3) ? pick(10) : pick(slots)); };   // (draws sample the same few textures)
    for (int i = 0; i < 40; i++) {
        const uint32_t slot = some_slot(), vt = pick(7);
        HandleHit h{};
        const int t = pick(textures + 3);
        h.tex = t < textures ? &w.textures[t] : nullptr;
        h.view = pick(6) ? (VkImageView)(uintptr_t)(0x20000 + i) : VK_NULL_HANDLE;
        h.version = pick(4) ? w.version[slot] : w.version[slot] + 1;
        h.destroy_gen = pick(5) ? w.dgen : w.dgen - 1;
        h.tex_epoch = pick(2) ? w.epoch : w.epoch - 1;
        h.level = pick(3);
        h.sampling = pick(4) != 0;
        sc.hits[draw_lean::hit_key(slot, vt, (uint32_t)ViewUse::Sample, 0)] = h;
    }
    const uint32_t stages[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    sc.st.vk_stage = stages[pick(3)];
    const int groups = index % 50 == 0 ? 30 : 1 + pick(6);   // (now and then more textures than the window asked ahead)
    uint32_t word = 0;
    for (int g = 0; g < groups; g++) {
        Shader::TextureDescriptor d;
        d.type = (Shader::TextureType)pick(7);
        d.is_depth = pick(4) == 0;
        d.count = 1 + pick(3);
        d.handle_program = pick(6) == 0 && d.count == 1 ? 3u : ~0U;   // (a handle program with one element: not a table)
        d.cbuf_offset = word * 4;
        d.pointer_mask = pick(5) == 0 ? (uint32_t)pick(8) : 0;
        word += d.count;
        sc.st.info.texture_descriptors.push_back(d);
    }
    sc.s.words.resize(word);
    for (auto& h : sc.s.words) h = pick(10) == 0 ? 0u : (uint32_t)pick(4096) << 20 | some_slot();
    sc.fp_on = pick(3) == 0;
    sc.binding0 = pick(8);
    return sc;
}

// ---- RX_VK_SMALL_COSTS: dyn_same_fast against dyn_same (both cut) ----
#include "dyn_same.inc"

int check_small_costs(std::mt19937& rng) {
    auto pick = [&](int n) { return (int)(rng() % (uint32_t)n); };
    const int nf = 13, nu = 35;
    int cases = 0;
    for (int i = 0; i < 20000; i++) {
        float f[nf], cf[nf];
        uint32_t u[nu], cu[nu];
        uint8_t b[160], cb[160];
        for (int k = 0; k < nf; k++) f[k] = cf[k] = (float)pick(5) - 2.0f;
        for (int k = 0; k < nu; k++) u[k] = cu[k] = (uint32_t)pick(3);
        const int nb = 1 + pick(160);
        for (int k = 0; k < nb; k++) b[k] = cb[k] = (uint8_t)pick(3);
        switch (pick(9)) {   // (the same, or one difference somewhere, the last word's and byte's included)
        case 0: break;
        case 1: cf[pick(nf)] += 1.0f; break;
        case 2: cu[pick(nu)] ^= 1u; break;
        case 3: cu[nu - 1] ^= 0x80000000u; break;
        case 4: cb[pick(nb)] ^= 1; break;
        case 5: cb[nb - 1] ^= 0x80; break;
        case 6: f[pick(nf)] = 0.0f, cf[0] = -0.0f, f[0] = 0.0f; break;   // (-0 and 0: the same by value)
        case 7: f[pick(nf)] = std::numeric_limits<float>::quiet_NaN(); break;   // (a NaN: never the same)
        case 8: { const int k = pick(nf); f[k] = cf[k] = std::numeric_limits<float>::quiet_NaN(); } break;
        }
        const int cnf = pick(20) ? nf : 0, cnu = pick(20) ? nu : 0, cnb = pick(20) ? nb : 0;   // (a count of 0: never the same)
        if (dyn_same(f, cnf, u, cnu, b, cnb, cf, cu, cb) != dyn_same_fast(f, cnf, u, cnu, b, cnb, cf, cu, cb)) {
            fprintf(stderr, "FAIL: dyn_same_fast answers otherwise than dyn_same (case %d)\n", i);
            return -1;
        }
        cases++;
    }
    for (uint64_t align = 16; align <= 4096; align <<= 1)   // (the mask, for the alignments a device gives and uniform_range's 16)
        for (int i = 0; i < 2000; i++) {
            const uint64_t off = (uint64_t)rng() << 8 ^ rng();
            if (draw_lean::aligned_pow2(off, align) != (off % align == 0) || !draw_lean::is_pow2(align)) {
                fprintf(stderr, "FAIL: the mask and %% disagree at offset %#llx, alignment %llu\n", (unsigned long long)off,
                        (unsigned long long)align);
                return -1;
            }
        }
    if (draw_lean::is_pow2(0) || draw_lean::is_pow2(48) || draw_lean::is_pow2(3)) {
        fprintf(stderr, "FAIL: is_pow2 takes a number that is not a power of two\n");
        return -1;
    }
    return cases;
}

// ---- RX_VK_INDEX_FIRST: the index the draw reads, the old way and the new ----
int check_index_first(std::mt19937& rng) {
    int cases = 0;
    for (int i = 0; i < 200000; i++) {
        const uint32_t unit = 1u << (rng() % 3);
        uint64_t off = ((uint64_t)rng() << 20 ^ rng()) & 0x3FFFFFFFFull;
        if (i % 3 == 0) off -= off % unit;   // (most offsets are whole indices: the console's are)
        const uint32_t count = rng() % 100000;
        uint32_t first = 0xDEAD;
        const bool ok = draw_lean::first_index_for(off, unit, count, &first);
        const bool whole = off % unit == 0, fits = off / unit + count <= 0xFFFFFFFFull;
        if (ok != (whole && fits)) {
            fprintf(stderr, "FAIL: first_index_for(%#llx, %u, %u) says %d\n", (unsigned long long)off, unit, count, (int)ok);
            return -1;
        }
        if (!ok) continue;
        // (index j of the draw: at bound offset + (firstIndex + j) * unit; bound at off with firstIndex 0 before)
        for (uint32_t j : {0u, 1u, count / 2, count ? count - 1 : 0u})
            if (0 + ((uint64_t)first + j) * unit != off + ((uint64_t)0 + j) * unit) {
                fprintf(stderr, "FAIL: index %u read at another byte (offset %#llx unit %u)\n", j, (unsigned long long)off, unit);
                return -1;
            }
        cases++;
    }
    uint32_t first = 0;
    if (draw_lean::first_index_for(0x400000000ull, 1, 0, &first) || !draw_lean::first_index_for(0xFFFFFFFFull, 1, 0, &first) ||
        first != 0xFFFFFFFFu || draw_lean::first_index_for(0xFFFFFFFFull, 1, 1, &first) || draw_lean::first_index_for(6, 4, 1, &first) ||
        draw_lean::first_index_for(8, 3, 1, &first)) {
        fprintf(stderr, "FAIL: first_index_for at the edges (32 bits, a part of an index, a size that is not 1, 2 or 4)\n");
        return -1;
    }
    return cases;
}

int check_switches() {
    struct { const char* v; int want; } cases[] = {{nullptr, 0}, {"", 0}, {"0", 0}, {"off", 0}, {"no", 0}, {"1", 1}, {"on", 1},
                                                   {"yes", 1}, {"check", 2}};
    for (const auto& c : cases)
        if (draw_lean::tex_lean_parse(c.v) != c.want) {
            fprintf(stderr, "FAIL: RX_VK_TEX_LEAN=%s read as %d\n", c.v ? c.v : "(unset)", draw_lean::tex_lean_parse(c.v));
            return 1;
        }
    return 0;
}

int main() {
    std::mt19937 rng(67);
    if (check_switches()) return 1;
    const int small = check_small_costs(rng);
    if (small < 0) return 1;
    const int index = check_index_first(rng);
    if (index < 0) return 1;
    printf("PASS: RX_VK_TEX_LEAN read as unset/0/off, 1/on, check; dyn_same_fast as dyn_same over %d cases; the mask as %% over "
           "every power-of-two alignment 16-4096; first_index_for over %d draws, the same bytes read\n", small, index);
    int compared = 0, textures = 0, misses = 0, touches = 0, ahead_off = 0;
    for (int i = 0; i < 3000; i++) {
        const Scenario sc = make(rng, i);
        if (!tex_lean_plain(sc.st.info)) {
            fprintf(stderr, "FAIL: a stage of no handle table is not plain\n");
            return 1;
        }
        const Result a = run(sc, false), b = run(sc, true);
        std::string why;
        if (!same(a, b, &why)) {
            fprintf(stderr, "FAIL: scenario %d: the lean way differs from the loop: %s\n", i, why.c_str());
            for (size_t k = 0; k < a.events.size() || k < b.events.size(); k++)
                fprintf(stderr, "  %-28s | %s\n", k < a.events.size() ? a.events[k].c_str() : "", k < b.events.size() ? b.events[k].c_str() : "");
            return 1;
        }
        compared++;
        textures += (int)a.imgs.size();
        for (const auto& e : a.events) misses += e.rfind("of ", 0) == 0, touches += e.rfind("quick", 0) == 0;
        ahead_off += a.imgs.size() > draw_lean::kPrefetchMost;
    }
    // (a handle table sends the stage to the loop)
    Shader::Info table;
    Shader::TextureDescriptor d;
    d.handle_program = 3, d.count = 4;
    table.texture_descriptors.push_back(d);
    if (tex_lean_plain(table)) {
        fprintf(stderr, "FAIL: a stage with a handle table is taken the lean way\n");
        return 1;
    }
    if (!misses || !touches || !ahead_off) {
        fprintf(stderr, "FAIL: the scenarios did not reach every path (misses %d, touches %d, stages past the window %d)\n", misses,
                touches, ahead_off);
        return 1;
    }
    printf("PASS: %d stages, %d textures: the lean way gives what the loop gives (image infos, writes, GPU-written list, "
           "fingerprint, %d texture_view_of misses, %d touches, handle_hits; %d stages past the prefetch window)\n",
           compared, textures, misses, touches, ahead_off);
    return 0;
}
