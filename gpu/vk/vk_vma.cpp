// Vulkan Memory Allocator (v3.3.0, from Eglantine's CPM cache: nx2/build.py adds its include path): images and
// device buffers are sub-allocated from large blocks (NVIDIA allows ~4096 allocations, and the imported pools
// already take about 2400 of them). Its Vulkan functions come from vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#ifdef __PROSPERO__
// (ps5, 2026-11-10: at the first 3D scene a VMA mutex's lock threw (runs 8 to 10: an abort) or never came back (runs 7 and
// 11's end: the presentation thread asleep for good), in VmaBlockVector::CommitAllocationRequest. VMA's mutex here is a
// pthread mutex made when its object is (not at its first lock, as std::mutex's is on this libc), with a mark beside it:
// a lock that fails or waits 10 s says so in the stage trail, with the mutex's address, the console's own word for it and
// whether the mark is still there (an object freed or written over), then goes on as a plain lock would)
#include <pthread.h>
#include <time.h>
#include <cstdint>
extern "C" void hos_ps5_trail(const char* fmt, ...);
extern "C" void hos_ps5_pthread_counts(char* buf, size_t cap);
extern "C" long hos_ps5_pthread_room(long most);
class Ps5VmaMutex {
public:
    Ps5VmaMutex() { made_ = pthread_mutex_init(&m_, nullptr); }
    ~Ps5VmaMutex() { pthread_mutex_destroy(&m_); mark_ = 0xdeadULL; }
    Ps5VmaMutex(const Ps5VmaMutex&) = delete;
    Ps5VmaMutex& operator=(const Ps5VmaMutex&) = delete;
    void Lock() {
        timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 10;
        int r = pthread_mutex_timedlock(&m_, &until);
        if (!r) return;
        static int told;
        if (told++ < 12)
            hos_ps5_trail("vma: mutex %p: lock answered %d (%s); the console's handle %p, made with %d, mark %#llx (0x564d41: intact)%s",
                          (void*)this, r, r == 110 || r == 60 ? "10 s without it" : "an error", *(void**)&m_, made_,
                          (unsigned long long)mark_, r == 60 || r == 110 ? "; waiting on" : "; going on WITHOUT the lock");
        if (told <= 2) {
            char po[900];
            hos_ps5_pthread_counts(po, sizeof po);
            hos_ps5_trail("vma: the thread library gives %ld more mutexes now (of 4096 asked); %s", hos_ps5_pthread_room(4096), po);
        }
        if (r == 60 || r == 110) pthread_mutex_lock(&m_);   // (ETIMEDOUT: FreeBSD's 60)
    }
    void Unlock() { pthread_mutex_unlock(&m_); }
    bool TryLock() { return pthread_mutex_trylock(&m_) == 0; }
private:
    pthread_mutex_t m_;
    int made_ = 0;
    uint64_t mark_ = 0x564d41ULL;
};
#define VMA_MUTEX Ps5VmaMutex
#endif
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_internal.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef LONGSHOT_ENV_H   // (env_on: the switches' one rule)
#include "longshot_env.h"
#endif
#include "vram_compact.h"

#ifdef __PROSPERO__
// (ps5, 2026-11-10: runs 8 to 10 ended in an abort at the first 3D scene: std::mutex::lock threw inside VMA's
// CommitAllocationRequest (texture_get -> create_image -> vmaCreateImage) and nobody caught it. What the exception says
// did not reach standard error's pipe, so the terminate handler writes it to the stage trail itself, with the error's number)
#include <exception>
#include <new>
#include <system_error>
#include <cerrno>
extern "C" void hos_ps5_trail(const char* fmt, ...);
// (2026-11-11, host/posix/ps5_platform.c) an uncaught std::bad_alloc ended the run of 11/11 at 20:04 with 304 MiB of direct memory
// free in pieces of 15 MiB at most: the floor guard's "STOPPING THE TITLE" line, with the heap's last refusal and the memory now,
// comes first (HOOKSHOT_PS5_HEAP_STOP=0: not written). Weak: a link without the console's host layer writes the crash line only
extern "C" void hos_ps5_heap_stop_line(const char* what) __attribute__((weak));
namespace {
[[noreturn]] void ps5_terminate() {
    static std::atomic<int> once{0};
    if (!once++) {
        if (std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::system_error& x) {
                {
                    char po[900];
                    hos_ps5_pthread_counts(po, sizeof po);
                    hos_ps5_trail("crash: the thread library gives %ld more mutexes now (of 4096 asked); %s", hos_ps5_pthread_room(4096), po);
                }
                hos_ps5_trail("crash: uncaught std::system_error: code %d (category %s): %s (EPERM %d EDEADLK %d EINVAL %d EAGAIN %d ENOMEM %d EBUSY %d)",
                              x.code().value(), x.code().category().name(), x.what(), EPERM, EDEADLK, EINVAL, EAGAIN, ENOMEM, EBUSY);
            } catch (const std::bad_alloc& x) {
                if (hos_ps5_heap_stop_line) hos_ps5_heap_stop_line(x.what());
                hos_ps5_trail("crash: uncaught C++ exception: %s", x.what());
            } catch (const std::exception& x) {
                hos_ps5_trail("crash: uncaught C++ exception: %s", x.what());
            } catch (...) {
                hos_ps5_trail("crash: uncaught C++ exception of an unknown type");
            }
        } else {
            hos_ps5_trail("crash: std::terminate called with no exception");
        }
    }
    abort();
}
const std::terminate_handler ps5_previous_terminate = std::set_terminate(ps5_terminate);
}   // namespace
#endif
#include "vk_work.h"   // (last, after the console's terminate handler, which calls no Vulkan: RX_VK_RECORDER=defer drains before the destructions and the invalidate below)

namespace mkwvk {

VmaAllocator vma;

bool vma_init(PFN_vkGetInstanceProcAddr gipa) {
    VmaVulkanFunctions f{};
    f.vkGetInstanceProcAddr = gipa;
    f.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
               (dev.memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    ci.physicalDevice = dev.physical;
    ci.device = dev.device;
    ci.instance = dev.instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &f;
    ci.preferredLargeHeapBlockSize = 64ull << 20;   // (64 MB blocks: Eglantine found 256 MB ones wasted VRAM)
    // (ps5, 2026-11-10) RX_VK_VMA_BLOCK_MB=<n>, 4 to 256: the blocks' size. On the console a block is direct memory from the moment
    // it is made, whatever it holds: 75 blocks of 2574 MB held 2023 MB of allocations where the title stopped at its memory floor
    if (const char* e = getenv("RX_VK_VMA_BLOCK_MB")) {
        ci.preferredLargeHeapBlockSize = (uint64_t)std::clamp(atoi(e), 4, 256) << 20;
        logf("vk: RX_VK_VMA_BLOCK_MB: blocks of %llu MB", (unsigned long long)(ci.preferredLargeHeapBlockSize >> 20));
    }
    return vmaCreateAllocator(&ci, &vma) == VK_SUCCESS;
}

// Images come from a pool of 128 MB blocks: on their own, VMA gave most of them dedicated memory (the driver prefers it
// for render targets, and anything over half a default block), a vkAllocateMemory and a kernel call for each, and
// the dynamic resolution makes a new set of render targets every few frames. Images too large for a block, or of a
// memory type the pool's is not, are allocated as before.
VmaPool image_pool = VK_NULL_HANDLE;
bool image_pool_tried = false;
// (RX_VK_IMAGE_BLOCK_MB=<n>, 16 and up: the block's size. VMA keeps one block empty for the next image, 128 MB of VRAM that a 64 MB block
// would halve, at twice the memory objects: the driver allows ~4096, the imported pools take ~2400)
// (ps5, 2026-11-10: read when the pool is made, not when the program starts: the console's settings file is read in main, after
// the static initializers, and the variable set there was never seen)
// (ps5, 2026-11-10: down to 1 MB, 16 before. The memory objects counted above are a PC driver's; the console's driver gives each one
// direct memory in 16 KiB pages and 2 MB of an address region of 256 GB. Small blocks go with RX_VK_IMAGE_DEDICATED_KB below)
uint64_t image_block() {
    static const uint64_t bytes = (getenv("RX_VK_IMAGE_BLOCK_MB") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_IMAGE_BLOCK_MB"))) : 128u) << 20;
    return bytes;
}

// (ps5, 2026-11-10) RX_VK_IMAGE_DEDICATED_KB=<n>: an image of about n KB or more gets a memory object of its own, not a place in a
// block. On the console a block is direct memory whatever it holds, and what images leave empty in their blocks was 490 MB where the
// title stopped at its memory floor (Kokiri Forest, 16 MB blocks: the textures' 49 blocks, 784 MB, held 505 MB of images in 254 free
// ranges; the render targets' 34 blocks, 544 MB, held 331 MB). An image's own memory object is given back whole when the image
// goes, and there it costs two kernel calls and a round-up to 16 KiB. Unset or 0: as before. Read at first use: the console's
// settings file is read in main, after the static initializers.
uint64_t image_dedicated_from() {
    static const uint64_t bytes = [] {
        const char* e = getenv("RX_VK_IMAGE_DEDICATED_KB");
        const uint64_t b = e ? (uint64_t)std::max(0, atoi(e)) << 10 : 0;
        if (b) logf("vk: RX_VK_IMAGE_DEDICATED_KB: images of about %llu KB and more in memory objects of their own", (unsigned long long)(b >> 10));
        return b;
    }();
    return bytes;
}
/// About what an image takes: its texels by its format's bytes (a compressed block's texel and an 8-bit one 1, two 8-bit channels
/// and 16-bit ones 2, four 16-bit channels 8, the rest taken for 4), a third more with mip levels. Only to tell large from small.
uint64_t image_bytes_about(const VkImageCreateInfo& ci) {
    uint64_t texels = (uint64_t)ci.extent.width * ci.extent.height * std::max(1u, ci.extent.depth) * std::max(1u, ci.arrayLayers);
    if (ci.mipLevels > 1) texels += texels / 3;
    const int f = (int)ci.format;
    const uint64_t each = (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ? 1
                        : (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB) ? 1
                        : (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) || (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) ||
                          f == VK_FORMAT_D16_UNORM ? 2
                        : (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) ? 8 : 4;
    return texels * each * std::max(1u, (unsigned)ci.samples);
}

// (longshot) and the render targets' images in blocks of their own (texture_get's for a target: OoT's transient ones, made and let go
// all the time). In the same blocks as the textures that stay, each one gone left a hole beside a texture that kept its block: the
// pool's 47-54 blocks (6-6.9 GB) held 1.9-3.6 GB of images in Kokiri Forest, VRAM sat at 93-98% of what the driver allows, and an
// image made there then took 30-70 ms (RTX 5070). RX_VK_TARGET_BLOCKS=0: in the textures' blocks, as before
VmaPool target_image_pool = VK_NULL_HANDLE;
bool target_pool_tried = false;
VmaPool images_pool();
VmaPool target_images_pool() {
    if (target_pool_tried) return target_image_pool;
    target_pool_tried = true;
    static const bool off = getenv("RX_VK_TARGET_BLOCKS") && !atoi(getenv("RX_VK_TARGET_BLOCKS"));
    if (off || !images_pool()) return VK_NULL_HANDLE;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = image_block();
    if (vmaCreatePool(vma, &pi, &target_image_pool) != VK_SUCCESS) target_image_pool = VK_NULL_HANDLE;
    logf("vk: render targets' images from blocks of their own (%llu MB, memory type %u)%s", (unsigned long long)(image_block() >> 20),
         type, target_image_pool ? "" : ": not made");
    return target_image_pool;
}

// (longshot, RX 7800 XT: part of a pool's blocks made at the start, not when the game first needs them. Each is a vkAllocateMemory of
// 128 MB, ~170 ms on AMD's driver, on the replay thread: ~70 of them in a walk across Hyrule Field, the frame rate dropping to the low
// 40s whenever new ground came in. In the pool the sampled textures are made in and no other, 15% of the card's memory, 2 to 24 blocks,
// none on an integrated GPU: vram_compact::blocks_at_start. RX_VK_IMAGE_PREALLOC=<blocks>: that many, 0: none)
static size_t blocks_at_start(bool holds_textures) {
    if (!holds_textures) return 0;
    uint64_t heap = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = std::max<uint64_t>(heap, test_vram_bytes(dev.memory.memoryHeaps[h].size));
    if (const char* e = getenv("RX_VK_IMAGE_PREALLOC")) return (size_t)std::max(0, atoi(e));
#ifdef __PROSPERO__
    // (ps5) none unless RX_VK_IMAGE_PREALLOC asks: on the console a block is direct memory from the moment it is made, whatever
    // it holds (RX_VK_VMA_BLOCK_MB, RX_VK_IMAGE_DEDICATED_KB), and AMD's Windows driver's ~170 ms a block is not this driver's
    (void)heap;
    return 0;
#else
    return (size_t)vram_compact::blocks_at_start(heap, image_block(), holds_textures,
                                                 dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
#endif
}
/// The pool with n blocks made now, or empty when there is no room for them (its blocks made as needed); the blocks made
static size_t make_pool(VmaPoolCreateInfo pi, size_t n, VmaPool& pool) {
    pi.minBlockCount = n;
    if (n && vmaCreatePool(vma, &pi, &pool) == VK_SUCCESS) return n;
    pi.minBlockCount = 0;
    if (vmaCreatePool(vma, &pi, &pool) != VK_SUCCESS) pool = VK_NULL_HANDLE;
    return 0;
}

VmaPool images_pool() {
    if (image_pool_tried) return image_pool;
    image_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (a representative image: the pool's memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = image_block();
    const size_t n = make_pool(pi, blocks_at_start(!vram_compact::enabled(getenv("RX_VK_COMPACT"))), image_pool);
    logf("vk: images from a pool of %llu MB blocks (memory type %u), %zu made at the start%s", (unsigned long long)(image_block() >> 20), type,
         n, image_pool ? "" : ": not made");
    return image_pool;
}

// (longshot) The sampled textures' own pool, when they are compacted (unless RX_VK_COMPACT=0): VMA's defragmentation asks to move any
// allocation of the pool it works on, and only vk_texture.cpp's images can be moved (it knows what they hold and who uses
// them) or kept from being freed while a pass is open. The render targets' images stay in their blocks above; DLSS's, the
// frame interpolation's and the scratch ones in the images' pool
VmaPool texture_pool = VK_NULL_HANDLE;
bool texture_pool_tried = false;
uint64_t texture_pool_kept = 0;   // (its blocks made at the start, which VMA keeps: bytes)
VmaPool textures_pool() {
    if (texture_pool_tried) return texture_pool;
    texture_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = image_block();
    texture_pool_kept = make_pool(pi, blocks_at_start(true), texture_pool) * image_block();
    logf("vk: textures from a pool of their own, %llu MB blocks (RX_VK_COMPACT), %llu made at the start%s",
         (unsigned long long)(image_block() >> 20), (unsigned long long)(texture_pool_kept / image_block()), texture_pool ? "" : ": not made");
    return texture_pool;
}
bool textures_pool_usage(uint64_t* block_bytes, uint64_t* free_bytes, uint64_t* kept_bytes) {
    if (!texture_pool) return false;
    VmaStatistics st{};
    vmaGetPoolStatistics(vma, texture_pool, &st);
    *block_bytes = st.blockBytes, *free_bytes = st.blockBytes - std::min(st.blockBytes, st.allocationBytes);
    if (kept_bytes) *kept_bytes = texture_pool_kept;
    return true;
}

// ---- the textures' pool compacted: VMA's defragmentation context and its open pass ----
namespace {
VmaDefragmentationContext compact_ctx = VK_NULL_HANDLE;
VmaDefragmentationPassMoveInfo compact_pass{};
bool compact_in_pass = false;
bool compact_over(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    VmaDefragmentationStats st{};
    vmaEndDefragmentation(vma, compact_ctx, &st);
    compact_ctx = VK_NULL_HANDLE;
    if (freed_bytes) *freed_bytes = st.bytesFreed;
    if (freed_blocks) *freed_blocks = st.deviceMemoryBlocksFreed;
    return false;
}
}
bool image_compact_begin(uint64_t max_bytes_a_pass, uint32_t max_moves_a_pass) {
    if (compact_ctx || !texture_pool) return false;
    VmaDefragmentationInfo di{};
    di.pool = texture_pool;
    di.maxBytesPerPass = max_bytes_a_pass;
    di.maxAllocationsPerPass = max_moves_a_pass;
    return vmaBeginDefragmentation(vma, &di, &compact_ctx) == VK_SUCCESS;
}
bool image_compact_active() { return compact_ctx != VK_NULL_HANDLE; }
bool image_compact_pass(std::vector<ImageMove>& moves, uint64_t* freed_bytes, uint32_t* freed_blocks) {
    moves.clear();
    if (!compact_ctx || compact_in_pass) return false;
    const VkResult r = vmaBeginDefragmentationPass(vma, compact_ctx, &compact_pass);
    if (r != VK_INCOMPLETE) return compact_over(freed_bytes, freed_blocks);   // (VK_SUCCESS: nothing left to move; or an error)
    compact_in_pass = true;
    for (uint32_t i = 0; i < compact_pass.moveCount; i++) moves.push_back({compact_pass.pMoves[i].srcAllocation});
    return true;
}
bool image_compact_bind(size_t i, VkImage image) {
    return compact_in_pass && i < compact_pass.moveCount &&
           vmaBindImageMemory(vma, compact_pass.pMoves[i].dstTmpAllocation, image) == VK_SUCCESS;
}
void image_compact_ignore(size_t i) {
    if (compact_in_pass && i < compact_pass.moveCount)
        compact_pass.pMoves[i].operation = VMA_DEFRAGMENTATION_MOVE_OPERATION_IGNORE;
}
bool image_compact_pass_end(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    if (!compact_ctx || !compact_in_pass) return false;
    compact_in_pass = false;
    if (vmaEndDefragmentationPass(vma, compact_ctx, &compact_pass) == VK_INCOMPLETE) return true;
    return compact_over(freed_bytes, freed_blocks);
}

bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // (in VRAM or not at all: an image VMA puts in system memory when VRAM is full is read over PCIe every frame,
    // and takes the RAM the game and everything else on the machine need)
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (!dedicated && image_dedicated_from() && image_bytes_about(ci) >= image_dedicated_from()) dedicated = true;   // (ps5: above)
    if (dedicated) {   // (a memory object of its own, which freeing gives back whole: a pool's block is kept by VMA when it empties)
        VmaAllocationCreateInfo dai = ai;
        dai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(vma, &ci, &dai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    }
    static const bool no_pool = env_on("RX_VK_NO_IMAGE_POOL");
    if (VmaPool pool = no_pool ? VK_NULL_HANDLE
                       : textures && textures_pool() ? textures_pool()
                       : target && target_images_pool() ? target_images_pool() : images_pool()) {
        VmaAllocationCreateInfo pai = ai;
        pai.pool = pool;
        pai.usage = VMA_MEMORY_USAGE_UNKNOWN;
        if (vmaCreateImage(vma, &ci, &pai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    }
    if (vmaCreateImage(vma, &ci, &ai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    if (ai.flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT) {
        static std::atomic<int> told_budget{0};
        if (told_budget++ < 10) logf("vk: image over budget, retrying without WITHIN_BUDGET");
        VmaAllocationCreateInfo vai = ai;
        vai.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        if (vmaCreateImage(vma, &ci, &vai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    }
    // (longshot) VRAM is full (a 4 GB card at a high resolution, #96): an image left unmade is a black screen, so it goes
    // to system memory instead, slow but drawn
    if (env_on("RX_VK_IMAGES_VRAM_ONLY")) return false;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vma, &ci, &sai, &image, &allocation, nullptr) != VK_SUCCESS) return false;
    static std::atomic<int> told{0};
    if (told++ < 4) logf("vk: VRAM full: a %ux%u image (format %d) in system memory", ci.extent.width, ci.extent.height, (int)ci.format);
    return true;
}
void destroy_image(VkImage image, VmaAllocation allocation) {
    vmaDestroyImage(vma, image, allocation);
}
uint64_t allocation_size(VmaAllocation allocation) {
    VmaAllocationInfo info{};
    if (allocation) vmaGetAllocationInfo(vma, allocation, &info);
    return info.size;
}
/// (longshot #1843) whether an allocation is in a device-local heap (VRAM; on the console, the GPU's direct memory)
bool allocation_in_vram(VmaAllocation allocation) {
    if (!allocation) return false;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, allocation, &info);
    return info.memoryType < dev.memory.memoryTypeCount &&
           (dev.memory.memoryHeaps[dev.memory.memoryTypes[info.memoryType].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
}
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer,
                   VmaAllocation& allocation, void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    // (a buffer with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT gets VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT from VMA:
    // vma_init sets VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT)
    static const bool blocks = env_on("RX_VK_BLOCK_BUFFERS");
    if (dedicated && !blocks) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    return true;
}
/// A buffer in cached host memory, mapped: written on one thread, read back on the CPU too (the captures: the game's
/// threads copy pages into it, the replay reads texture pages out of it and the GPU copies the rest from it)
bool create_buffer_cached(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation,
                          void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;   // (its own memory object: no part of a 64 MB block left unused)
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    return true;
}
bool host_cached(VmaAllocation allocation) {
    VkMemoryPropertyFlags f = 0;
    if (allocation) vmaGetAllocationMemoryProperties(vma, allocation, &f);
    return (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
}
bool host_coherent(VmaAllocation allocation) {
    VkMemoryPropertyFlags f = 0;
    if (allocation) vmaGetAllocationMemoryProperties(vma, allocation, &f);
    return (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}
void flush_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, 0, size);
}
void flush_mapped_range(VmaAllocation allocation, uint64_t offset, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, offset, size);
}
void invalidate_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaInvalidateAllocation(vma, allocation, 0, size);
}
bool flush_mapped_checked(VmaAllocation allocation, uint64_t size) {
    return !size || vmaFlushAllocation(vma, allocation, 0, size) == VK_SUCCESS;
}
bool invalidate_mapped_checked(VmaAllocation allocation, uint64_t size) {
    return !size || vmaInvalidateAllocation(vma, allocation, 0, size) == VK_SUCCESS;
}
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation) {
    vmaDestroyBuffer(vma, buffer, allocation);
}
bool create_device_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    return vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, nullptr) == VK_SUCCESS;
}
/// (longshot, RX_VK_MIRROR_SPARSE) A buffer with sparse residency and no memory: its blocks are bound one by one
/// (vk_memory.cpp, sparse_mirror.h). req: its size, block (alignment) and memory types
bool create_sparse_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VkMemoryRequirements& req) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        return false;
    }
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    return true;
}
/// One block of device memory for a sparse buffer (req's alignment and memory types), from VMA's blocks: counted in its
/// budget as the rest of the device's memory is
bool alloc_sparse_block(const VkMemoryRequirements& req, VmaAllocation& allocation, VkDeviceMemory& memory, uint64_t& offset) {
    VkMemoryRequirements r = req;
    r.size = req.alignment;
    VmaAllocationCreateInfo ai{};
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    VmaAllocationInfo info{};
    if (vmaAllocateMemory(vma, &r, &ai, &allocation, &info) != VK_SUCCESS) {
        allocation = nullptr;
        return false;
    }
    memory = info.deviceMemory;
    offset = info.offset;
    return true;
}
void free_sparse_block(VmaAllocation allocation) {
    if (allocation) vmaFreeMemory(vma, allocation);
}
void destroy_sparse_buffer(VkBuffer buffer) {   // (its blocks' memory is the caller's to free, after)
    if (buffer) vkDestroyBuffer(dev.device, buffer, nullptr);
}
/// (every heap: what VMA has in blocks and allocations there, what the process uses of it, its budget; one line)
/// The image pools: their blocks, the images in them, and the free ranges between them (every 600 presents, with the heaps)
void vma_log_image_blocks() {
    for (VmaPool pool : {image_pool, target_image_pool, texture_pool}) {
        if (!pool) continue;
        VmaDetailedStatistics d{};
        vmaCalculatePoolStatistics(vma, pool, &d);
        logf("vk: image blocks%s: %u blocks %llu MB, %u images %llu MB; %u free ranges, the largest %llu MB",
             pool == target_image_pool ? " (render targets)" : pool == texture_pool ? " (textures, compacted)" : "", d.statistics.blockCount,
             (unsigned long long)(d.statistics.blockBytes >> 20), d.statistics.allocationCount,
             (unsigned long long)(d.statistics.allocationBytes >> 20), d.unusedRangeCount,
             (unsigned long long)(d.unusedRangeSizeMax >> 20));
    }
}
void vma_log_heaps() {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    char line[1024];
    int n = snprintf(line, sizeof line, "vk: memory heaps:");
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount && n < 900; h++)
        n += snprintf(line + n, sizeof line - n, " heap %u%s: %u blocks %llu MB, %u allocations %llu MB; used %llu of %llu MB;", h,
                      (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : " (system)",
                      b[h].statistics.blockCount, (unsigned long long)(b[h].statistics.blockBytes >> 20),
                      b[h].statistics.allocationCount, (unsigned long long)(b[h].statistics.allocationBytes >> 20),
                      (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20));
    logf("%s", line);
}
#ifdef __PROSPERO__
/// (ps5, 2026-11-10: the memory watch's line, host/hos_thread.c: what VMA holds in each heap, for the moment the title ends
/// itself. From any thread; nothing before the allocator is made)
extern "C" void rx_ps5_vk_memory_note(char* buf, size_t cap) {
    if (!cap) return;
    buf[0] = 0;
    if (!vma) return;
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    int n = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount && n < (int)cap - 120; h++)
        n += snprintf(buf + n, cap - n, " heap %u%s: VMA %u blocks %llu MB holding %u allocations %llu MB, the driver counts %llu of %llu MB;", h,
                      (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : " (system)",
                      b[h].statistics.blockCount, (unsigned long long)(b[h].statistics.blockBytes >> 20),
                      b[h].statistics.allocationCount, (unsigned long long)(b[h].statistics.allocationBytes >> 20),
                      (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20));
}
#endif
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0, f = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            u += b[h].usage, t += b[h].budget;
            // (the free space in VMA's blocks: the driver counts it as the process's, new allocations take it first)
            f += b[h].statistics.blockBytes - std::min(b[h].statistics.blockBytes, b[h].statistics.allocationBytes);
        }
    // (a test: RX_VK_TEST_ALLOWED_MB=<n> reports what the driver allows as at most n MB, so a 16 GB card plays the eviction and
    // keep-rendered paths an 8 GB card takes when its VRAM fills (the rainbow textures of issue 993 were never seen here otherwise))
    static const uint64_t test_allowed = getenv("RX_VK_TEST_ALLOWED_MB") ? (uint64_t)atoi(getenv("RX_VK_TEST_ALLOWED_MB")) << 20 : 0;
    if (test_allowed && t > test_allowed) t = test_allowed;
    // (and RX_VK_TEST_ALLOWED_FILE=<file>: the same bound read from that file's number of MB, looked at again once a second, so
    // a run can shrink the allowance in play, as Windows does when another program takes VRAM: an RX 6600 went from 7378 to
    // 5940 MB allowed in Hyrule Field (#1025). A missing file or 0 leaves the driver's)
    static const char* test_file = getenv("RX_VK_TEST_ALLOWED_FILE");
    if (test_file) {
        static std::atomic<uint64_t> file_allowed{0};
        static std::atomic<int64_t> read_at{0};
        const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - read_at.load(std::memory_order_relaxed) >= 1000) {
            read_at.store(now, std::memory_order_relaxed);
            uint64_t mb = 0;
            if (FILE* fp = fopen(test_file, "r")) {
                unsigned long long v = 0;
                if (fscanf(fp, "%llu", &v) == 1) mb = v;
                fclose(fp);
            }
            if ((mb << 20) != file_allowed.load(std::memory_order_relaxed))
                logf("vk: test allowance (RX_VK_TEST_ALLOWED_FILE): %llu MB%s", (unsigned long long)mb, mb ? "" : " (the driver's)");
            file_allowed.store(mb << 20, std::memory_order_relaxed);
        }
        const uint64_t fa = file_allowed.load(std::memory_order_relaxed);
        if (fa && t > fa) t = fa;
    }
    // (and RX_VK_TEST_ALLOWED_FOLLOW=1: an allowance the count is past is reported as the count, as AMD's Windows driver does
    // (#933, #1025), so the rule below can be seen at work on any card)
    static const bool test_follow = env_on("RX_VK_TEST_ALLOWED_FOLLOW");
    if (test_follow && (test_allowed || test_file) && t && u > t) t = u;
    // (longshot) an allowance the driver gives as the count itself is past: AMD's Windows driver, once a process is over what it
    // allows, reports the allowance as what the process uses. An RX 6600 at 3.4 of 7.2 GB entered Hyrule Field and sat at 5940 of
    // 5940 MB; another went from 5.3 of 6.5 GB to 6.6 of 6.6, the allowance rising with the count. Taken as it is, the count is
    // within it: room in the blocks kept the 97% squeeze waiting and the hold's line rose with the count, while the driver paged:
    // frames of 1.2-15.7 s (#933, #1025). vram_compact::past_allowance makes it a tenth under the count, so every rule sees the
    // count past it and gives back until the driver reports an allowance over the count again. RX_VK_ALLOWED_AT_COUNT=0: as before
#ifdef __PROSPERO__   // (ps5: off unless RX_VK_ALLOWED_AT_COUNT=1: the console's texture rules (97%, its memory guard) read the driver
                     // as before #1091; its allowance near the count would otherwise drop by a tenth of what is used, ~500 MB)
    static const bool at_count_rule = getenv("RX_VK_ALLOWED_AT_COUNT") && atoi(getenv("RX_VK_ALLOWED_AT_COUNT")) != 0;
#else
    static const bool at_count_rule = !getenv("RX_VK_ALLOWED_AT_COUNT") || atoi(getenv("RX_VK_ALLOWED_AT_COUNT")) != 0;
#endif
    if (at_count_rule) {
        const uint64_t was = t;
        t = vram_compact::past_allowance(u, t);
        static std::atomic<int> told{0};
        if (t != was && told.fetch_add(1, std::memory_order_relaxed) < 8)
            logf("vk: the driver's allowance is the count itself (%llu MB used of %llu): past it, taken as %llu MB "
                 "(RX_VK_ALLOWED_AT_COUNT=0: as the driver says)", (unsigned long long)(u >> 20), (unsigned long long)(was >> 20),
                 (unsigned long long)(t >> 20));
    }
    *used = u, *budget = t;
    if (block_free) *block_free = f;
    return t != 0;
}

/// The system-memory heaps the GPU reaches: what the process uses there (the imported guest memory too: pinned by the
/// driver) and what the driver allows it, asked now (a new frame index makes VMA ask the driver again); false when unknown
namespace {
std::atomic<uint32_t> budget_frame{0};   // (VMA's frame index: a new one makes it ask the driver for the budgets again)
}
bool vram_usage_now(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    return vram_usage(used, budget) && dev.memory_budget;
}
bool system_heap_usage(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (!(dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) u += b[h].usage, t += b[h].budget;
    *used = u, *budget = t;
    return dev.memory_budget && t != 0;
}

} // namespace mkwvk
