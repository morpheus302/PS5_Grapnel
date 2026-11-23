// Hookshot on the PS5: what the Vulkan back end asks of a device (gpu/vk/vk_main.cpp create_device, vk_shader.cpp's
// shader profile, vk_formats.cpp's formats), checked against one device with its own function pointers. The smoke title
// (vk_smoke.cpp) runs it on the console after Hookshot's own mkwvk_init; pc_census.cpp runs it on the PC against any ICD
// (the PS5's RADV built for Linux: probe/build/cache-test2).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

struct CensusCounts {
    int pass = 0, missing_needed = 0, missing_used = 0, missing_diag = 0, info = 0;
};

/// Each line through say(). gipa: the ICD's or loader's vkGetInstanceProcAddr; the instance must be 1.3 or newer.
/// display: also check the instance-level VK_KHR_display path (the console's surface).
CensusCounts vk_census(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
                       void (*say)(const char* line));
