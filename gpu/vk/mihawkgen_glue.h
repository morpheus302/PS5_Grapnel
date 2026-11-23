// (ps5) PS5_MihawkGen's generator (third_party/mihawkgen, unmodified) as the back end builds it: nx2/build.py compiles its
// sources with this header forced in, after vk_loader.h, so its Vulkan calls go through the back end's loaded functions as
// every unit of gpu/vk, and three of them through the back end (vk_framegen.cpp, mihawk_glue):
//  - vkAllocateMemory and vkFreeMemory: counted, so the start's line says what the generator holds (its images and its weights
//    are memory of its own, not VMA's: one allocation each);
//  - vkCreateComputePipelines: one at a time through extern_guarded_compute, as the back end's own pipelines (a driver fault
//    while compiling is caught and said, not a crash), and counted.
// Nothing else of its code changes. Only its four units see this header.
#pragma once
#include "vk_loader.h"

namespace mihawk_glue {
VkResult allocate(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* callbacks, VkDeviceMemory* out);
void free_memory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks);
VkResult compute_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos,
                           const VkAllocationCallbacks* callbacks, VkPipeline* out);
}   // namespace mihawk_glue

#define vkAllocateMemory mihawk_glue::allocate
#define vkFreeMemory mihawk_glue::free_memory
#define vkCreateComputePipelines mihawk_glue::compute_pipelines
