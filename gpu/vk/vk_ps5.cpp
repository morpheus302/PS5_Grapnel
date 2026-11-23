// (PS5) What the console needs of the Vulkan back end that no other platform does: compiled on every platform (nx2/build.py
// takes gpu/vk/*.cpp) and empty except for __PROSPERO__. (The compile threads' stacks are vk_internal.h's CompileThread.)
//
// The display: no window system on the console. With an MkwVkDisplay for start() (vk_api.h), the surface is a
// VK_KHR_display plane over VideoOut, as PS5_Vulkan's smoke title (PPSA99014, radv/radv_smoke.c) and ProsperoEden
// (headless/vulkan_surface.cpp) make theirs: the one display, one of its modes, plane 0, identity transform, opaque. The
// swapchain is then vk_main.cpp's create_swapchain, unchanged: the mode's size is the surface's currentExtent, FIFO is the
// only present mode (paced by VideoOut's flips), 2 to 5 images (Hookshot asks for 4), B8G8R8A8_UNORM, and TRANSFER_DST,
// which the present blit needs, is among the usages (PS5_Mesa src/vulkan/wsi/wsi_common_videoout.c). The modes
// (PS5_VulkanTemplate skills/ps5-homebrew/references/vulkan-on-radv.md): 3840x2160, 2560x1440 and 1920x1080, largest first,
// each size's 119.88 Hz mode (when param.json declares the high frame rate bit and the display takes it) before its
// 59.94 Hz one; VideoOut scales the mode to the screen. (The host may instead pass its MkwVkWindow, as on Linux, and make
// the surface itself: vk_main.cpp's start() takes either.)
#if defined(__PROSPERO__)
#include "vk_internal.h"

#include <pthread_np.h>
#include <cstdlib>
#include <vector>

namespace mkwvk {

size_t ps5_thread_stack_bytes() {
    pthread_attr_t a;
    size_t size = 0;
    if (pthread_attr_init(&a) == 0) {
        if (pthread_attr_get_np(pthread_self(), &a) == 0) pthread_attr_getstacksize(&a, &size);
        pthread_attr_destroy(&a);
    }
    return size;
}

bool ps5_display_surface(const MkwVkDisplay* want, VkExtent2D* extent) {
    if (want && want->version != MKWVK_DISPLAY_VERSION) {
        logf("vk: unsupported PS5 display version %u (expected %u)", want->version, MKWVK_DISPLAY_VERSION);
        return false;
    }
    if (!vkGetPhysicalDeviceDisplayPropertiesKHR || !vkGetDisplayModePropertiesKHR || !vkCreateDisplayPlaneSurfaceKHR) {
        logf("vk: VK_KHR_display's commands are missing");
        return false;
    }
    uint32_t n = 1;
    VkDisplayPropertiesKHR display{};
    VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(dev.physical, &n, &display);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) {
        logf("vk: no display (VkResult %d, %u displays)", (int)r, n);
        return false;
    }
    // (every mode, however many the TV offers: PPSA99014's display test had room for two, and failed on a TV with more)
    std::vector<VkDisplayModePropertiesKHR> modes;
    do {
        n = 0;
        r = vkGetDisplayModePropertiesKHR(dev.physical, display.display, &n, nullptr);
        if (r != VK_SUCCESS || !n) break;
        modes.resize(n);
        r = vkGetDisplayModePropertiesKHR(dev.physical, display.display, &n, modes.data());
        modes.resize(n);
    } while (r == VK_INCOMPLETE);
    if (r != VK_SUCCESS || modes.empty()) {
        logf("vk: the display %s has no modes (VkResult %d)", display.displayName ? display.displayName : "?", (int)r);
        return false;
    }
    // the mode: the size asked for (0: the largest, the first), at the refresh asked for (0: 59.94 Hz, which every
    // size has; 119.88 Hz needs the title's param.json to declare it, or the swapchain is refused)
    const uint32_t want_w = want ? want->width : 0, want_h = want ? want->height : 0;
    const uint32_t want_hz = want && want->refresh_mhz ? want->refresh_mhz : 59940;
    const VkDisplayModePropertiesKHR* pick = nullptr;
    int best = -1;
    for (const auto& m : modes) {
        const VkExtent2D e = m.parameters.visibleRegion;
        const bool size_ok = (!want_w && !want_h) ? (e.width == modes[0].parameters.visibleRegion.width &&
                                                     e.height == modes[0].parameters.visibleRegion.height)
                                                  : (e.width == want_w && e.height == want_h);
        const int hz_off = std::abs((int)m.parameters.refreshRate - (int)want_hz);
        const int score = (size_ok ? 1 << 20 : 0) - std::min(hz_off, (1 << 20) - 1);
        if (score > best) best = score, pick = &m;
    }
    for (const auto& m : modes)
        logf("vk: display mode %ux%u at %.3f Hz%s", m.parameters.visibleRegion.width, m.parameters.visibleRegion.height,
             m.parameters.refreshRate / 1000.0, &m == pick ? " (chosen)" : "");
    VkDisplaySurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    si.displayMode = pick->displayMode;
    si.planeIndex = 0;
    si.planeStackIndex = 0;
    si.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    si.globalAlpha = 1.0f;
    si.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    si.imageExtent = pick->parameters.visibleRegion;
    r = vkCreateDisplayPlaneSurfaceKHR(dev.instance, &si, nullptr, &dev.surface);
    if (r != VK_SUCCESS) {
        logf("vk: vkCreateDisplayPlaneSurfaceKHR failed (VkResult %d)", (int)r);
        return false;
    }
    *extent = si.imageExtent;
    logf("vk: the console's display (%s): %ux%u at %.3f Hz", display.displayName ? display.displayName : "VideoOut",
         extent->width, extent->height, pick->parameters.refreshRate / 1000.0);
    return true;
}

}  // namespace mkwvk
#endif
