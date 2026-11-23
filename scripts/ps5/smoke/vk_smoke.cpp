// Hookshot on the PS5, phase 3: the Vulkan back end's smoke title (PPSA99144, "Hookshot Vulkan Smoke").
//
// It links gpu/vk, Eden's recompiler and sirit as the game will (static archives, RADV linked in, no loader) and runs, on
// the console:
//   1. mkwvk_init: Hookshot's own start, unchanged but for its __PROSPERO__ branches: the instance (VK_KHR_surface and
//      VK_KHR_display), its device choice, and its device with every extension and feature it asks for where present.
//      Its log lines are Hookshot's own ("vk: ..."), with the stacks its compile threads get (vk_internal.h);
//   2. the census (vk_census.cpp): each extension, feature, property and format the back end uses, PASS or MISSING;
//   3. the display: Hookshot's PS5 surface (vk_ps5.cpp, VK_KHR_display over VideoOut) and its own create_swapchain,
//      then 180 frames (3 s) cleared through its swapchain images with the usage it asks for (TRANSFER_DST), paced
//      by the display.
// Every line goes to klog (prefix [hookshot-vk-smoke]) and to /app0/vk-smoke.txt (the title folder:
// /data/homebrew/PPSA99144/vk-smoke.txt). It ends by itself through the shell (no exit()), in about 5 s.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_internal.h"
#include "vk_census.h"

#include <ps5platform/klog.h>

#include <sys/stat.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" {
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char* path, const char* const* argv);
int sceKernelUsleep(unsigned int microseconds);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
}

namespace {

const char* const kResults = "/app0/vk-smoke.txt";
FILE* results;
int passed, failed;

void say(const char* l) {
    std::fprintf(stderr, "%s\n", l);
    std::fflush(stderr);
    if (results) std::fprintf(results, "%s\n", l), std::fflush(results);
}
void sayf(const char* fmt, ...) {
    char b[600];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    say(b);
}
void check(bool ok, const char* what) {
    (ok ? passed : failed)++;
    sayf("%s %s", ok ? "PASS" : "FAIL", what);
}
double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// (the host functions mkwvk_init's start reaches: the log only. The others are the game's (guest memory, NVN layouts, the
// pad) and are not called before start(), which this title does not call)
const MkwVkHost* smoke_host() {
    static MkwVkHost h{};
    h.version = MKWVK_API_VERSION;
    h.log = [](const char* l) { sayf("[hookshot] %s", l); };
    h.reserved_cpus = [] { return (uint64_t)0; };
    h.now_ns = [] { return (int64_t)(now() * 1e9); };
    h.aspect_ratio = [] { return 0.0f; };
    h.pad_raw = [] { return 0u; };
    return &h;
}

/// Hookshot's swapchain (made by its create_swapchain), cleared and presented: what the game's presents do, without the game
bool present_frames(int frames) {
    using namespace mkwvk;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = dev.queue_family;
    VkCommandPool pool;
    if (vkCreateCommandPool(dev.device, &pi, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev.device, &ai, &cb);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired;
    vkCreateSemaphore(dev.device, &si, nullptr, &acquired);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence done;
    vkCreateFence(dev.device, &fi, nullptr, &done);
    int shown = 0;
    double t0 = 0, worst = 0, last = 0;
    for (int frame = 0; frame < frames; frame++) {
        uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(dev.device, dev.swapchain, 2000000000ull, acquired, VK_NULL_HANDLE, &index);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            sayf("display: vkAcquireNextImageKHR returned %d at frame %d", (int)r, frame);
            break;
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = dev.swap_images[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
        // (a slow sweep from Hookshot's green to blue, so a person in front of the TV sees frames change)
        const float t = (float)frame / (float)frames;
        VkClearColorValue colour{};
        colour.float32[0] = 0.05f;
        colour.float32[1] = 0.55f * (1.0f - t) + 0.15f * t;
        colour.float32[2] = 0.20f * (1.0f - t) + 0.75f * t;
        colour.float32[3] = 1.0f;
        vkCmdClearColorImage(cb, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier2(cb, &di);
        vkEndCommandBuffer(cb);
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = acquired;
        wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = dev.swap_rendered[index];   // (the swapchain's own per-image semaphores, as the game's presents use)
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cb;
        VkSubmitInfo2 s{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        s.waitSemaphoreInfoCount = 1, s.pWaitSemaphoreInfos = &wait;
        s.commandBufferInfoCount = 1, s.pCommandBufferInfos = &cbi;
        s.signalSemaphoreInfoCount = 1, s.pSignalSemaphoreInfos = &signal;
        if ((r = vkQueueSubmit2(dev.queue, 1, &s, done)) != VK_SUCCESS) {
            sayf("display: vkQueueSubmit2 returned %d at frame %d", (int)r, frame);
            break;
        }
        VkPresentInfoKHR pr{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pr.waitSemaphoreCount = 1;
        pr.pWaitSemaphores = &dev.swap_rendered[index];
        pr.swapchainCount = 1;
        pr.pSwapchains = &dev.swapchain;
        pr.pImageIndices = &index;
        r = vkQueuePresentKHR(dev.queue, &pr);
        vkWaitForFences(dev.device, 1, &done, VK_TRUE, 2000000000ull);
        vkResetFences(dev.device, 1, &done);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            sayf("display: vkQueuePresentKHR returned %d at frame %d", (int)r, frame);
            break;
        }
        const double at = now();
        if (frame == 30) t0 = at;   // (the first frames fill the swapchain: timed after them)
        if (frame > 30) worst = std::max(worst, at - last);
        last = at;
        shown++;
    }
    vkDeviceWaitIdle(dev.device);
    if (shown > 31)
        sayf("display: frames 31-%d took %.3f s, %.2f ms a frame on average, the slowest %.2f ms", shown - 1, last - t0,
             (last - t0) * 1000.0 / (shown - 31), worst * 1000.0);
    vkDestroyFence(dev.device, done, nullptr);
    vkDestroySemaphore(dev.device, acquired, nullptr);
    vkDestroyCommandPool(dev.device, pool, nullptr);
    return shown == frames;
}

}  // namespace

int main(int, char**) {
    const int klog = ps5_klog_capture_stderr("[hookshot-vk-smoke] ");
    results = std::fopen(kResults, "w");
    sayf("Hookshot Vulkan smoke (PPSA99144): gpu/vk, Eden's recompiler and sirit linked with RADV; standard error to klog: %s; "
         "results in %s: %s", klog == 0 ? "yes" : "no", kResults, results ? "yes" : "no");
    sayf("splash dismissed: %d", sceSystemServiceHideSplashScreen());

    // 1. Hookshot's own start
    const double t0 = now();
    const MkwVkRenderer* table = mkwvk_init(smoke_host());
    sayf("mkwvk_init: %.0f ms", (now() - t0) * 1000.0);
    check(table != nullptr, "mkwvk_init: Hookshot's instance, device choice and device (gpu/vk/vk_main.cpp)");
    using mkwvk::dev;
    if (dev.device) {
        sayf("hookshot device: %s; mesh shaders %d (fallback %d), host memory import %d, pipeline libraries %d / fast-linked %d, "
             "push descriptors %d (at most %u), null descriptors %d, dynamic state 3 %d (polygon %d clamp %d blend %d a2c %d), "
             "custom borders %d, depth clip control %d, uint8 indices %d, conditional rendering %d, provoking vertex %d, "
             "divisors %d, stencil export %d, memory budget %d, 2D views of 3D %d, sparse images %d buffers %d, "
             "checkpoints %d, buffer markers %d, device fault %d, cooperative matrices %d, subgroups pinned on %#x",
             dev.props.deviceName, dev.mesh_shader, dev.mesh_fallback, dev.external_memory_host, dev.pipeline_library,
             dev.graphics_pipeline_library, dev.push_descriptor, dev.max_push_descriptors, dev.null_descriptor,
             dev.dynamic_state3, dev.eds3_polygon, dev.eds3_clamp, dev.eds3_blend, dev.eds3_a2c, dev.custom_border_color,
             dev.depth_clip_control, dev.index_uint8, dev.conditional_rendering, dev.provoking_vertex, dev.vertex_divisor,
             dev.stencil_export, dev.memory_budget, dev.view_2d_of_3d, dev.sparse_images, dev.sparse_buffers, dev.checkpoints,
             dev.buffer_marker, dev.device_fault, dev.coop_matrix, (unsigned)dev.sg32_stages);
        check(dev.mesh_shader, "Hookshot enabled VK_EXT_mesh_shader with mesh and task shaders");
        check(dev.external_memory_host, "Hookshot enabled VK_EXT_external_memory_host (guest memory imported in place)");
        check(dev.sg32_stages != 0, "Hookshot pinned its programs to 32-lane subgroups");
    }

    // 2. the census, on the device Hookshot chose
    if (dev.instance && dev.physical) {
        const CensusCounts c = vk_census(vk_icdGetInstanceProcAddr, dev.instance, dev.physical, say);
        check(c.missing_needed == 0, "census: nothing the back end needs is missing");
    }

    // 3. the display, through Hookshot's PS5 surface and its own swapchain
    if (dev.device) {
        const MkwVkDisplay want{MKWVK_DISPLAY_VERSION, 0, 0, 0};
        VkExtent2D extent{};
        const bool surface = mkwvk::ps5_display_surface(&want, &extent);
        check(surface, "display: a VK_KHR_display plane surface over VideoOut (gpu/vk/vk_ps5.cpp)");
        VkBool32 support = VK_FALSE;
        if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.queue_family, dev.surface, &support);
        check(support == VK_TRUE, "display: Hookshot's queue family presents to it");
        const bool chain = support && mkwvk::create_swapchain(extent.width, extent.height);
        check(chain, "display: Hookshot's create_swapchain (gpu/vk/vk_main.cpp) on the display");
        if (chain) {
            check(present_frames(180), "display: 180 frames cleared through Hookshot's swapchain images and presented");
            sceKernelUsleep(1000000);
        }
    }

    sayf("Hookshot Vulkan smoke ends: %d passed, %d failed", passed, failed);
    if (results) {
        std::fclose(results);
        chmod(kResults, 0666);   // (readable and replaceable over FTP)
    }
    return failed ? 1 : 0;
}

// The title ends by asking the shell to close it: exit() and returning from _start both kill it, and klog then reports a
// crash. PS5_Vulkan's CRT (tooling/native/app_crt.cpp) calls this after main returns.
extern "C" void catchReturnFromMain(int status) {
    std::fprintf(stderr, "[hookshot-vk-smoke] exit status %d: asking the shell to close the title\n", status);
    std::fflush(nullptr);
    (void)sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sceKernelUsleep(100000);
}
