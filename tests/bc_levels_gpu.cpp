// (ps5 lot 60) The ASTC-to-BC3 re-encode on a real GPU, run by the production code: tests/test_bc_levels_gpu.py inserts
// gpu/vk/vk_texture.cpp's upload_astc_gpu, bc_scratch_make, bc_scratch_layout, bc_encode_into, level_geo and the scratch's
// globals at the marker below, from this tree or from another commit (lot 57's, before the groups). Around them: a Vulkan
// device of this PC, the real decoder and encoder (the SPIR-V the renderer has built in), synthetic ASTC textures in the
// Switch 2 block-linear layout, and stubs for what the uploads take from the rest of the renderer. For each texture it
// writes the BC3 image read back, level after level, to --out, and prints the full barriers and dispatches its upload
// recorded. The Python check runs it with the switches set different ways and compares the bytes.
// (ps5 lot 61) It also runs lot 61's one pass (RX_VK_BC_ONE_PASS=1: bc_one_pass and its globals are cut out too, the pipelines
// made here from the three SPIR-V headers it adds), and with --time N times each upload on the GPU (N uploads of a texture in
// one command buffer between two timestamps); TEST_SYNC_CS=1 puts an execution barrier after every dispatch, as the console's
// driver waits for the compute engine after each one (RADV_PS5_SYNC_CS=1).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <dlfcn.h>
#include "astc_decoder_comp_spv.h"
#include "bc_encoder_comp_spv.h"
#if __has_include("astc_decoder_levels_comp_spv.h")
#include "astc_decoder_levels_comp_spv.h"
#include "bc_encoder_levels_comp_spv.h"
#include "bc_encoder_levels_direct_comp_spv.h"
#define HAVE_ONE_PASS 1
#endif
#include <utility>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

#define APIS(X) X(vkCreateInstance) X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkEnumerateDeviceExtensionProperties) X(vkGetDeviceProcAddr)
#define DEV_APIS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkQueueSubmit) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateComputePipelines) X(vkResetCommandPool) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2)
#define DECLARE(n) PFN_##n n;
APIS(DECLARE)
DEV_APIS(DECLARE)
PFN_vkCreateDevice vkCreateDevice;
#undef DECLARE
// (the command-buffer calls the production code makes: counted as the renderer's vk_work.h counts them)
PFN_vkCmdPipelineBarrier2 p_vkCmdPipelineBarrier2;
PFN_vkCmdBindPipeline p_vkCmdBindPipeline;
PFN_vkCmdPushDescriptorSetKHR p_vkCmdPushDescriptorSetKHR;
PFN_vkCmdPushConstants p_vkCmdPushConstants;
PFN_vkCmdDispatch p_vkCmdDispatch;
PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage;
PFN_vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer;
PFN_vkCmdBlitImage p_vkCmdBlitImage;
uint64_t work_epoch = 0;
struct Counts { uint64_t barriers = 0, decodes = 0, encodes = 0, copies = 0, regions = 0; } counts;
VkPipeline bound = VK_NULL_HANDLE;
VkPipeline astc_pipe = VK_NULL_HANDLE, bc_pipe = VK_NULL_HANDLE, bc7_pipe = VK_NULL_HANDLE;
VkPipeline one_pass_decoder = VK_NULL_HANDLE;   // (lot 61's, counted as a decode: set in main)
bool sync_cs = false;   // (TEST_SYNC_CS=1)
void vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* d) { work_epoch++; p_vkCmdPipelineBarrier2(c, d); }
void vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint b, VkPipeline p) { bound = p; p_vkCmdBindPipeline(c, b, p); }
void vkCmdPushDescriptorSetKHR(VkCommandBuffer c, VkPipelineBindPoint b, VkPipelineLayout l, uint32_t s, uint32_t n,
                               const VkWriteDescriptorSet* w) { p_vkCmdPushDescriptorSetKHR(c, b, l, s, n, w); }
void vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags f, uint32_t o, uint32_t n, const void* v) {
    p_vkCmdPushConstants(c, l, f, o, n, v);
}
void vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
    work_epoch++;
    (bound == astc_pipe || (bound && bound == one_pass_decoder) ? counts.decodes : counts.encodes)++;
    p_vkCmdDispatch(c, x, y, z);
    if (sync_cs) {   // (the console's RADV_PS5_SYNC_CS=1: the next work starts once this dispatch is done)
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
        p_vkCmdPipelineBarrier2(c, &d);
    }
}
void vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r) {
    work_epoch++, counts.copies++, counts.regions += n;
    p_vkCmdCopyBufferToImage(c, b, i, l, n, r);
}
void vkCmdBlitImage(VkCommandBuffer c, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl, uint32_t n, const VkImageBlit* r,
                    VkFilter f) { work_epoch++; p_vkCmdBlitImage(c, a, al, b, bl, n, r, f); }

// ---- what the production code takes from the rest of the renderer ----
struct Dev {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    uint32_t max_push_descriptors = 0;
} dev;
struct VmaAllocation_T { VkDeviceMemory memory; VkDeviceSize size; bool vram; };
using VmaAllocation = VmaAllocation_T*;
struct NvnDesc { uint32_t width, height, depth; uint32_t block_w, block_h, levels; };
struct Layout { uint64_t layer_size = 0; };
struct Texture {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t layers = 1, levels = 1, width = 1, height = 1, skip = 0;
    int block_w = 4, block_h = 4, bpb = 16;
    bool bc = true, bc_resized = false;
    uint64_t gpu = 0;
    Layout L;
    NvnDesc d{};
    VkImage image = VK_NULL_HANDLE;
    VkImageUsageFlags usage = 0;
};
/// The Switch 2 block-linear layout of a level: block heights as the NVN driver picks them (the largest of up to 16 GOBs
/// the level's block rows need), one GOB deep, the levels one after the other
unsigned tile_h_of(const NvnDesc* d, uint32_t level) {
    const uint32_t h = std::max(1u, d->height >> level), th = (h + d->block_h - 1) / d->block_h;
    unsigned t = 0;
    while (t < 4 && (8u << t) < th) t++;
    return t;
}
uint64_t swizzled_of(const NvnDesc* d, uint32_t level) {
    const uint32_t w = std::max(1u, d->width >> level), h = std::max(1u, d->height >> level);
    const uint64_t tw = (w + d->block_w - 1) / d->block_w, th = (h + d->block_h - 1) / d->block_h;
    const unsigned t = tile_h_of(d, level);
    const uint64_t gobs_x = (tw * 16 + 63) / 64, rows_y = (th + (8ull << t) - 1) >> (3 + t);
    return gobs_x * rows_y * (512ull << t);
}
struct Host {
    uint64_t tex_level_offset(const NvnDesc* d, uint32_t level) {
        uint64_t at = 0;
        for (uint32_t l = 0; l < level; l++) at += swizzled_of(d, l);
        return at;
    }
    void tex_level_tiles(const NvnDesc* d, uint32_t level, unsigned* tile_h, unsigned* tile_d) {
        *tile_h = tile_h_of(d, level), *tile_d = 0;
    }
} host_object, *host = &host_object;

std::vector<uint8_t> guest;   // (the textures' ASTC blocks: a Texture's gpu is its offset here)
using ScratchBytes = std::vector<uint8_t>;
const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, ScratchBytes&) {
    CHECK(gpu + n <= guest.size());
    return guest.data() + gpu;
}
VkPhysicalDeviceMemoryProperties memory_props{};
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
VmaAllocation bind_memory(VkMemoryRequirements r, VkMemoryPropertyFlags want) {
    const uint32_t t = memory_type(r.memoryTypeBits, want);
    if (t == UINT32_MAX) return nullptr;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size, ai.memoryTypeIndex = t;
    auto* a = new VmaAllocation_T{VK_NULL_HANDLE, r.size, (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0};
    if (vkAllocateMemory(dev.device, &ai, nullptr, &a->memory) != VK_SUCCESS) { delete a; return nullptr; }
    return a;
}
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool = false, bool = false, bool = false) {
    if (vkCreateImage(dev.device, &ci, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements r{};
    vkGetImageMemoryRequirements(dev.device, image, &r);
    allocation = bind_memory(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    return allocation && vkBindImageMemory(dev.device, image, allocation->memory, 0) == VK_SUCCESS;
}
void destroy_image(VkImage image, VmaAllocation allocation) {
    if (image) vkDestroyImage(dev.device, image, nullptr);
    if (allocation) vkFreeMemory(dev.device, allocation->memory, nullptr), delete allocation;
}
bool create_device_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, bool = false) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size, bi.usage = usage;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements r{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &r);
    allocation = bind_memory(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    return allocation && vkBindBufferMemory(dev.device, buffer, allocation->memory, 0) == VK_SUCCESS;
}
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool, VkBuffer& buffer, VmaAllocation& allocation, void**, bool d = false) {
    return create_device_buffer(size, usage, buffer, allocation, d);
}
[[maybe_unused]] bool allocation_in_vram(VmaAllocation a) { return a && a->vram; }
[[maybe_unused]] uint64_t allocation_size(VmaAllocation a) { return a ? a->size : 0; }
bool low_vram_texture_mips() { return getenv("TEST_LOW_VRAM") && atoi(getenv("TEST_LOW_VRAM")); }
bool env_on(const char* name) { const char* e = getenv(name); return e && *e && strcmp(e, "0"); }
[[maybe_unused]] void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::fprintf(stderr, "log: ");
    std::vfprintf(stderr, fmt, a);
    std::fprintf(stderr, "\n");
    va_end(a);
}
uint64_t rec_value = 1;
VkCommandBuffer current_cmd = VK_NULL_HANDLE;
VkCommandBuffer upload_cmd() { return current_cmd; }
struct InitRedirect { void* to = nullptr; explicit InitRedirect(bool = true) {} };
struct GprofSpan { const char* label; int items = 1; uint64_t bytes = 0; GprofSpan(VkCommandBuffer, const char* l) : label(l) {} };
namespace gt { enum Kind { UP_ASTC_GPU, BC_ENCODE }; }
void gt_note_tex(gt::Kind, const Texture&) {}
void descriptors_disturbed() {}
// (as vk_texture.cpp's: a barrier right after another on the same command buffer, with no work between, is left out)
VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;
uint64_t last_barrier_epoch = ~0ull;
void full_barrier(VkCommandBuffer cmd, const char* = nullptr) {
    if (cmd == last_barrier_cmd && work_epoch == last_barrier_epoch) return;
    counts.barriers++;
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    last_barrier_cmd = cmd, last_barrier_epoch = work_epoch;
}
VkPipelineLayout astc_layout = VK_NULL_HANDLE, bc_layout = VK_NULL_HANDLE;
bool astc_ready() { return true; }
bool bc7_enabled() { return false; }
[[maybe_unused]] VkImageView astc_level_view(Texture*, uint32_t, uint32_t) { return VK_NULL_HANDLE; }
struct Staging { VkBuffer buffer = VK_NULL_HANDLE; uint64_t offset = 0; uint8_t* data = nullptr; VmaAllocation allocation = nullptr; };
VkBuffer staging_buffer = VK_NULL_HANDLE;
uint8_t* staging_data = nullptr;
uint64_t staging_size = 0, staging_used = 0;
Staging staging(uint64_t n, uint64_t align = 256) {
    const uint64_t at = (staging_used + align - 1) / align * align;
    if (at + n > staging_size) return {};
    staging_used = at + n;
    return {staging_buffer, at, staging_data + at, nullptr};
}
void flush_mapped_range(VmaAllocation, uint64_t, uint64_t) {}   // (host-coherent here)
int64_t st_bytes = 0;
uint64_t st_astc = 0;
thread_local const char* astc_fail = nullptr;
bool bc_scratch2_make() { return false; }   // (the resized textures' scratch: none of these is resized)
uint64_t gpu_completed_now() { return 0; }   // (the views of the one pass go after each texture's fence: main)
bool bc_one_pass_ready();

// PRODUCTION_BC

#ifdef HAVE_ONE_PASS
#ifdef ONE_PASS_CUT
/// (lot 61) the one-pass pipelines, as bc_one_pass_ready makes them
bool bc_one_pass_ready() { return op_dec_pipe && op_enc_pipe; }
void make_one_pass(VkPipeline (*make)(const uint32_t*, size_t, VkPipelineLayout),
                   VkPipelineLayout (*layout)(const std::vector<std::pair<uint32_t, VkDescriptorType>>&, uint32_t)) {
    constexpr VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    op_dec_layout = layout({{0, SB}, {1, SI}, {2, SB}}, sizeof(OnePassDecPush));
    op_dec_pipe = make(ASTC_DECODER_LEVELS_COMP_SPV, sizeof ASTC_DECODER_LEVELS_COMP_SPV, op_dec_layout);
    op_enc_layout = layout({{0, SI}, {1, SB}, {2, SB}}, sizeof(OnePassEncPush));
    op_enc_pipe = make(BC_ENCODER_LEVELS_COMP_SPV, sizeof BC_ENCODER_LEVELS_COMP_SPV, op_enc_layout);
    one_pass_decoder = op_dec_pipe;
    if (bc_one_pass_copy()) return;
    std::vector<std::pair<uint32_t, VkDescriptorType>> b{{0, SI}, {2, SB}};
    for (uint32_t i = 0; i < kOnePassLevels; i++) b.push_back({3 + i, SI});
    op_direct_layout = layout(b, sizeof(OnePassEncPush));
    op_direct_pipe = make(BC_ENCODER_LEVELS_DIRECT_COMP_SPV, sizeof BC_ENCODER_LEVELS_DIRECT_COMP_SPV, op_direct_layout);
}
void one_pass_views_done() { bc_one_pass_views_reap(~0ull); }
#else
bool bc_one_pass_ready() { return false; }
void make_one_pass(VkPipeline (*)(const uint32_t*, size_t, VkPipelineLayout),
                   VkPipelineLayout (*)(const std::vector<std::pair<uint32_t, VkDescriptorType>>&, uint32_t)) {}
void one_pass_views_done() {}
#endif
#else
bool bc_one_pass_ready() { return false; }
#endif

[[maybe_unused]] void bc_encode_resized(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, Texture&, uint32_t, uint32_t) {
    std::fprintf(stderr, "FAIL: a resized texture in the check\n");
    std::exit(1);
}

int skip(const char* why) {
    std::printf("SKIP: %s\n", why);
    return 0;
}
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
VkPipeline make_pipeline(const uint32_t* code, size_t bytes, VkPipelineLayout layout) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes, mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(dev.device, &mi, nullptr, &m) == VK_SUCCESS);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m, ci.stage.pName = "main", ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    CHECK(vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p) == VK_SUCCESS);
    vkDestroyShaderModule(dev.device, m, nullptr);
    return p;
}
VkPipelineLayout make_layout(VkDescriptorType b0, VkDescriptorType b1, uint32_t push) {
    const VkDescriptorSetLayoutBinding b[2] = {{0, b0, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, b1, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2, li.pBindings = b;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) == VK_SUCCESS);
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(dev.device, &pi, nullptr, &l) == VK_SUCCESS);
    return l;
}
VkPipelineLayout make_layout_n(const std::vector<std::pair<uint32_t, VkDescriptorType>>& bindings, uint32_t push) {
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (const auto& [n, type] : bindings) b.push_back({n, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = (uint32_t)b.size(), li.pBindings = b.data();
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) == VK_SUCCESS);
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(dev.device, &pi, nullptr, &l) == VK_SUCCESS);
    return l;
}
/// Deterministic ASTC blocks: one in four a void-extent block of one colour, the others random bits (many decode to the
/// decoder's error colour, many to real endpoints and weights: the encoder gets both)
uint64_t rng_state = 0;
uint32_t rng() {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng_state >> 33);
}
void fill_astc(uint8_t* p, uint64_t bytes, uint64_t seed) {
    rng_state = seed * 0x9E3779B97F4A7C15ull + 1;
    for (uint64_t at = 0; at + 16 <= bytes; at += 16) {
        uint8_t* b = p + at;
        if (rng() % 4 == 0) {
            const uint8_t head[8] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            std::memcpy(b, head, 8);
            for (int i = 8; i < 16; i++) b[i] = (uint8_t)rng();
        } else {
            for (int i = 0; i < 16; i++) b[i] = (uint8_t)rng();
        }
    }
}

int main(int argc, char** argv) {
    const char* want_device = nullptr;
    const char* out_path = nullptr;
    int time_n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) want_device = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) time_n = atoi(argv[++i]);
        else { std::fprintf(stderr, "usage: %s --out FILE [--device NAME] [--time N]\n", argv[0]); return 2; }
    }
    sync_cs = getenv("TEST_SYNC_CS") && atoi(getenv("TEST_SYNC_CS"));
    CHECK(out_path);
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!loader) return skip("Vulkan loader unavailable");
    auto gip = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    bool ok = gip != nullptr;
#define LOAD(n) n = reinterpret_cast<PFN_##n>(dlsym(loader, #n)); ok = ok && n;
    APIS(LOAD)
#undef LOAD
    if (!ok) return skip("Vulkan entry points unavailable");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return skip("no Vulkan 1.3 instance");
    uint32_t np = 0;
    vkEnumeratePhysicalDevices(instance, &np, nullptr);
    std::vector<VkPhysicalDevice> pds(np);
    vkEnumeratePhysicalDevices(instance, &np, pds.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        if (want_device && lower(p.deviceName).find(lower(want_device)) == std::string::npos) continue;
        VkPhysicalDeviceFeatures f{};
        vkGetPhysicalDeviceFeatures(pd, &f);
        if (!f.textureCompressionBC) continue;
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ext(ne);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, ext.data());
        bool push = false;
        for (auto& e : ext) push |= !strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        if (!push) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
        for (uint32_t q = 0; q < nq && family == UINT32_MAX; q++)
            if (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT) family = q;
        if (family == UINT32_MAX) continue;
        physical = pd, dev.props = p;
        break;
    }
    if (!physical) return skip(want_device ? "no Vulkan 1.3 device of that name with BC textures and push descriptors"
                                           : "no Vulkan 1.3 device with BC textures and push descriptors");
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    std::printf("GPU: %s; Vulkan %u.%u\n", dev.props.deviceName, VK_VERSION_MAJOR(dev.props.apiVersion),
                VK_VERSION_MINOR(dev.props.apiVersion));
    vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(gip(instance, "vkCreateDevice"));
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family, qci.queueCount = 1, qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.textureCompressionBC = VK_TRUE;
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(physical, &have);
    f2.features.shaderStorageImageWriteWithoutFormat = have.shaderStorageImageWriteWithoutFormat;
    f2.features.robustBufferAccess = have.robustBufferAccess;   // (the renderer runs with it: the decoder's reads past a level)
    const char* dext[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2, dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci, dci.enabledExtensionCount = 1,
    dci.ppEnabledExtensionNames = dext;
    CHECK(vkCreateDevice(physical, &dci, nullptr, &dev.device) == VK_SUCCESS);
#define LOADD(n) n = reinterpret_cast<PFN_##n>(vkGetDeviceProcAddr(dev.device, #n)); CHECK(n);
    DEV_APIS(LOADD)
#undef LOADD
#define LOADC(n) p_##n = reinterpret_cast<PFN_##n>(vkGetDeviceProcAddr(dev.device, #n)); CHECK(p_##n);
    LOADC(vkCmdPipelineBarrier2) LOADC(vkCmdBindPipeline) LOADC(vkCmdPushDescriptorSetKHR) LOADC(vkCmdPushConstants)
    LOADC(vkCmdDispatch) LOADC(vkCmdCopyBufferToImage) LOADC(vkCmdCopyImageToBuffer) LOADC(vkCmdBlitImage)
#undef LOADC
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev.device, family, 0, &queue);
    // (the renderer's two pipelines, as astc_ready and bc_ready make them)
    astc_layout = make_layout(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(AstcPush));
    astc_pipe = make_pipeline(ASTC_DECODER_COMP_SPV, sizeof ASTC_DECODER_COMP_SPV, astc_layout);
    bc_layout = make_layout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizeof(BcPush));
    bc_pipe = make_pipeline(BC_ENCODER_COMP_SPV, sizeof BC_ENCODER_COMP_SPV, bc_layout);
    dev.max_push_descriptors = 32;   // (VK_KHR_push_descriptor's least)
#ifdef HAVE_ONE_PASS
    make_one_pass(make_pipeline, make_layout_n);   // (lot 61's three: none when the code cut out has no one pass)
#endif
    // (staging and read-back: one host-visible buffer each)
    auto host_buffer = [&](uint64_t size, VkBufferUsageFlags usage, VkBuffer& b, uint8_t*& data) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size, bi.usage = usage;
        CHECK(vkCreateBuffer(dev.device, &bi, nullptr, &b) == VK_SUCCESS);
        VkMemoryRequirements r{};
        vkGetBufferMemoryRequirements(dev.device, b, &r);
        VmaAllocation a = bind_memory(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CHECK(a && vkBindBufferMemory(dev.device, b, a->memory, 0) == VK_SUCCESS);
        void* p = nullptr;
        CHECK(vkMapMemory(dev.device, a->memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS);
        data = (uint8_t*)p;
    };
    staging_size = 96ull << 20;
    host_buffer(staging_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, staging_buffer, staging_data);
    VkBuffer readback = VK_NULL_HANDLE;
    uint8_t* readback_data = nullptr;
    const uint64_t readback_size = 64ull << 20;
    host_buffer(readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, readback_data);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(dev.device, &pci, nullptr, &pool) == VK_SUCCESS);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(dev.device, &fci, nullptr, &fence) == VK_SUCCESS);
    FILE* out = fopen(out_path, "wb");
    CHECK(out);

    // (the textures: the console's single-layer BC3 textures are up to 1024 a side, most 512 or less (RX_VK_TEX_MAX_DIM=512),
    // sizes not a multiple of 4, every ASTC block size the game uses; a level over 1024 (the big scratch in a group); layer arrays,
    // which #1843 runs 8 layers at a time)
    struct Spec { const char* name; uint32_t w, h, layers, bw, bh; };
    const Spec specs[] = {
        {"512x512 8x8", 512, 512, 1, 8, 8},       {"341x170 6x6", 341, 170, 1, 6, 6},
        {"1024x512 4x4", 1024, 512, 1, 4, 4},     {"2048x256 8x8", 2048, 256, 1, 8, 8},
        {"1365x1365 8x8", 1365, 1365, 1, 8, 8},   {"300x200 10x10", 300, 200, 1, 10, 10},
        {"64x64 4x4", 64, 64, 1, 4, 4},           {"256x128 5x5", 256, 128, 1, 5, 5},
        {"1000x1000 12x12", 1000, 1000, 1, 12, 12}, {"4x4 4x4", 4, 4, 1, 4, 4},
        {"682x682 x9 8x8", 682, 682, 9, 8, 8},     {"512x512 x3 6x6", 512, 512, 3, 6, 6},
        {"128x96 x17 4x4", 128, 96, 17, 4, 4},
    };
    uint64_t seed = 1;
    for (const Spec& sp : specs) {
        Texture tex;
        tex.width = sp.w, tex.height = sp.h, tex.layers = sp.layers;
        tex.levels = 1 + (uint32_t)std::floor(std::log2((double)std::max(sp.w, sp.h)));
        tex.block_w = (int)sp.bw, tex.block_h = (int)sp.bh;
        tex.d = {sp.w, sp.h, sp.layers, sp.bw, sp.bh, tex.levels};
        tex.L.layer_size = host->tex_level_offset(&tex.d, tex.levels);
        const uint64_t total = tex.L.layer_size * tex.layers;
        guest.assign(total, 0);
        fill_astc(guest.data(), total, seed++);
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D, ii.format = VK_FORMAT_BC3_UNORM_BLOCK, ii.extent = {sp.w, sp.h, 1};
        ii.mipLevels = tex.levels, ii.arrayLayers = tex.layers, ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        // (as create() makes a BC3 texture: raw views of its blocks, R32G32B32A32_UINT, as storage)
        ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        tex.usage = ii.usage;
        VmaAllocation ta = nullptr;
        CHECK(create_image(ii, tex.image, ta));
        CHECK(vkResetCommandPool(dev.device, pool, 0) == VK_SUCCESS);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        CHECK(vkAllocateCommandBuffers(dev.device, &cai, &cmd) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
        current_cmd = cmd, last_barrier_cmd = VK_NULL_HANDLE, staging_used = 0;
        {   // (the texture's image in GENERAL, as create() leaves a new one)
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = tex.image, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex.levels, 0, tex.layers};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &di);
        }
        const Counts before = counts;
        CHECK(upload_astc_gpu(tex, total, nullptr));
        CHECK(!astc_fail);
        const Counts after = counts;
        full_barrier(cmd);
        uint64_t at = 0;
        std::vector<std::pair<uint64_t, uint64_t>> level_bytes;
        for (uint32_t l = 0; l < tex.levels; l++) {
            const uint32_t w = std::max(1u, sp.w >> l), h = std::max(1u, sp.h >> l);
            const uint64_t n = (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * 16 * tex.layers;
            CHECK(at + n <= readback_size);
            const VkBufferImageCopy c{at, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, tex.layers}, {0, 0, 0}, {w, h, 1}};
            p_vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_GENERAL, readback, 1, &c);
            level_bytes.push_back({at, n});
            at += n;
        }
        VkMemoryBarrier2 hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, hb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo hd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        hd.memoryBarrierCount = 1, hd.pMemoryBarriers = &hb;
        p_vkCmdPipelineBarrier2(cmd, &hd);
        CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1, si.pCommandBuffers = &cmd;
        CHECK(vkResetFences(dev.device, 1, &fence) == VK_SUCCESS);
        CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
        CHECK(vkWaitForFences(dev.device, 1, &fence, VK_TRUE, 60000000000ull) == VK_SUCCESS);
#ifdef HAVE_ONE_PASS
        one_pass_views_done();
#endif
        // (what the encoder had to work with: distinct BC3 blocks in level 0)
        std::vector<std::string> distinct;
        for (uint64_t b = 0; b < std::min<uint64_t>(level_bytes[0].second, 4096 * 16); b += 16)
            distinct.emplace_back((const char*)readback_data + level_bytes[0].first + b, 16);
        std::sort(distinct.begin(), distinct.end());
        const size_t kinds = (size_t)(std::unique(distinct.begin(), distinct.end()) - distinct.begin());
        std::fprintf(out, "texture %s levels %u layers %u bytes %llu\n", sp.name, tex.levels, tex.layers, (unsigned long long)at);
        fwrite(readback_data, 1, at, out);
        std::printf("%-18s %2u levels x%-2u: %4llu full barriers, %4llu decodes, %4llu encodes, %3llu copies (%llu regions); "
                    "level 0: %zu distinct BC3 blocks of the first %llu\n",
                    sp.name, tex.levels, tex.layers, (unsigned long long)(after.barriers - before.barriers),
                    (unsigned long long)(after.decodes - before.decodes), (unsigned long long)(after.encodes - before.encodes),
                    (unsigned long long)(after.copies - before.copies), (unsigned long long)(after.regions - before.regions), kinds,
                    (unsigned long long)std::min<uint64_t>(level_bytes[0].second / 16, 4096));
        vkDestroyImage(dev.device, tex.image, nullptr);
        vkFreeMemory(dev.device, ta->memory, nullptr);
        delete ta;
    }
    fclose(out);
    if (time_n > 0) {
        // (--time N: the GPU time of an upload, N uploads of a texture in one command buffer between two timestamps, the second
        // round of two counted (the first wakes the GPU's clocks up). The sizes the console's square re-makes: single-layer BC3,
        // 576 and 768 a side, ASTC 6x6 and 8x8, 10 levels; and around them)
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, qs.data());
        if (!qs[family].timestampValidBits) {
            std::printf("time: no timestamps on this queue\nDONE\n");
            return 0;
        }
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP, qpi.queryCount = 2;
        VkQueryPool qpool = VK_NULL_HANDLE;
        CHECK(vkCreateQueryPool(dev.device, &qpi, nullptr, &qpool) == VK_SUCCESS);
        const Spec tspecs[] = {{"768x768 8x8", 768, 768, 1, 8, 8}, {"576x576 6x6", 576, 576, 1, 6, 6},
                               {"512x512 8x8", 512, 512, 1, 8, 8}, {"512x512 4x4", 512, 512, 1, 4, 4},
                               {"1024x1024 8x8", 1024, 1024, 1, 8, 8}, {"256x256 6x6", 256, 256, 1, 6, 6}};
        for (const Spec& sp : tspecs) {
            Texture tex;
            tex.width = sp.w, tex.height = sp.h, tex.layers = sp.layers;
            tex.levels = 1 + (uint32_t)std::floor(std::log2((double)std::max(sp.w, sp.h)));
            tex.block_w = (int)sp.bw, tex.block_h = (int)sp.bh;
            tex.d = {sp.w, sp.h, sp.layers, sp.bw, sp.bh, tex.levels};
            tex.L.layer_size = host->tex_level_offset(&tex.d, tex.levels);
            const uint64_t total = tex.L.layer_size * tex.layers;
            guest.assign(total, 0);
            fill_astc(guest.data(), total, seed++);
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D, ii.format = VK_FORMAT_BC3_UNORM_BLOCK, ii.extent = {sp.w, sp.h, 1};
            ii.mipLevels = tex.levels, ii.arrayLayers = tex.layers, ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT;
            tex.usage = ii.usage;
            VmaAllocation ta = nullptr;
            CHECK(create_image(ii, tex.image, ta));
            double us = 0;
            Counts per{};
            for (int round = 0; round < 2; round++) {
                CHECK(vkResetCommandPool(dev.device, pool, 0) == VK_SUCCESS);
                VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                cai.commandPool = pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
                VkCommandBuffer cmd = VK_NULL_HANDLE;
                CHECK(vkAllocateCommandBuffers(dev.device, &cai, &cmd) == VK_SUCCESS);
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
                current_cmd = cmd, last_barrier_cmd = VK_NULL_HANDLE, staging_used = 0;
                VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = tex.image, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex.levels, 0, tex.layers};
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
                p_vkCmdPipelineBarrier2(cmd, &di);
                vkCmdResetQueryPool(cmd, qpool, 0, 2);
                full_barrier(cmd);
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool, 0);
                const Counts before = counts;
                for (int i = 0; i < time_n; i++) CHECK(upload_astc_gpu(tex, total, nullptr));
                const Counts after = counts;
                full_barrier(cmd);
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qpool, 1);
                CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1, si.pCommandBuffers = &cmd;
                CHECK(vkResetFences(dev.device, 1, &fence) == VK_SUCCESS);
                CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
                CHECK(vkWaitForFences(dev.device, 1, &fence, VK_TRUE, 60000000000ull) == VK_SUCCESS);
#ifdef HAVE_ONE_PASS
                one_pass_views_done();
#endif
                uint64_t ts[2] = {};
                CHECK(vkGetQueryPoolResults(dev.device, qpool, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
                us = (double)(ts[1] - ts[0]) * dev.props.limits.timestampPeriod / 1000.0 / time_n;
                per = {(after.barriers - before.barriers) / time_n, (after.decodes - before.decodes) / time_n,
                       (after.encodes - before.encodes) / time_n, (after.copies - before.copies) / time_n,
                       (after.regions - before.regions) / time_n};
            }
            std::printf("time %-14s %2u levels: %8.1f us an upload (%d in a command buffer%s): %llu decodes, %llu encodes, "
                        "%llu copies (%llu regions), %llu full barriers an upload\n", sp.name, tex.levels, us, time_n,
                        sync_cs ? ", an execution barrier after each dispatch" : "", (unsigned long long)per.decodes,
                        (unsigned long long)per.encodes, (unsigned long long)per.copies, (unsigned long long)per.regions,
                        (unsigned long long)per.barriers);
            vkDestroyImage(dev.device, tex.image, nullptr);
            vkFreeMemory(dev.device, ta->memory, nullptr);
            delete ta;
        }
        vkDestroyQueryPool(dev.device, qpool, nullptr);
    }
    std::printf("DONE\n");
    return 0;
}
