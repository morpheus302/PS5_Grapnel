// Hookshot on the PS5: the smoke title's census (vk_census.cpp) on the PC, through the system's Vulkan loader, against
// whichever ICD VK_ICD_FILENAMES names: the PS5's RADV built for Linux (PS5_Mesa 1ba9874f47, PS5 winsys host model,
// probe/build/cache-test2/icd_libcxx.json) reports what the console's driver reports, without a console.
//
//   pc_census [hookshot_vk.so]   with a Linux hookshot_vk.so (the AppImage's), its mkwvk_init runs first: Hookshot's own
//                                instance, device choice and device, as the game makes them, on that ICD
//   PC_CENSUS_API=<n>            the MKWVK_API_VERSION that .so was built with (v2.0.7's AppImage: 6; its MkwVkHost starts
//                                with the same version and log fields, the only ones mkwvk_init's device path uses)
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_census.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "vk_api.h"

static void say(const char* line) {
    std::puts(line);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) return say("no libvulkan.so.1"), 2;
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
    if (argc > 1) {   // (Hookshot's own path first)
        void* so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        auto init = so ? (mkwvk_init_fn)dlsym(so, "mkwvk_init") : nullptr;
        if (!init) {
            std::printf("no mkwvk_init in %s: %s\n", argv[1], dlerror());
        } else {
            MkwVkHost h{};
            h.version = getenv("PC_CENSUS_API") ? (uint32_t)atoi(getenv("PC_CENSUS_API")) : MKWVK_API_VERSION;
            h.log = [](const char* l) { std::printf("[hookshot_vk] %s\n", l); };
            const MkwVkRenderer* r = init(&h);
            std::printf("%s mkwvk_init: %s\n", r ? "PASS   " : "FAIL   ", r ? "instance, device choice and device made" : "null");
        }
    }
    auto create = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    auto enum_ext = (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    enum_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> all(n);
    enum_ext(nullptr, &n, all.data());
    std::vector<const char*> exts;
    for (const auto& e : all)
        if (!strcmp(e.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) || !strcmp(e.extensionName, VK_KHR_DISPLAY_EXTENSION_NAME))
            exts.push_back(e.extensionName);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "hookshot-vk-census";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    VkInstance inst;
    if (create(&ci, nullptr, &inst) != VK_SUCCESS) return say("vkCreateInstance failed"), 1;
    auto enum_pd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    enum_pd(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    enum_pd(inst, &n, pds.data());
    if (pds.empty()) return say("no physical device"), 1;
    const CensusCounts c = vk_census(gipa, inst, pds[0], say);
    return c.missing_needed ? 1 : 0;
}
