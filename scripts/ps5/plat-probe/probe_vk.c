/* The platform probe's Vulkan part (risk 4, docs/ps5-port.md M15): guest memory backed by host/posix/ps5_window.c in
   64 KiB units of direct memory, each its own allocation, imported into RADV with VK_EXT_external_memory_host as
   gpu/vk/vk_memory.cpp imports guest pools, written by a compute shader and checked by the CPU. RADV is PS5_Vulkan's stock
   release archive, reached as Hookshot reaches it on the console: vk_icdGetInstanceProcAddr, no loader. On the console
   RADV's import grants the range GPU access with sceKernelMprotect (PS5_Mesa winsys/ps5 radv_ps5_memory_grant_gpu): one
   call over 1,024 separate mappings here. SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat_probe.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "probe_import_spv.h"   /* (build-plat-probe.sh: glslangValidator, probe_import.comp) */

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipeline_layout;
static VkPipeline pipeline;
static VkCommandPool pool;
static VkFence fence;
static VkDeviceSize import_alignment;

#define F(name) static PFN_##name name
F(vkCreateInstance); F(vkDestroyInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceProperties2); F(vkGetPhysicalDeviceQueueFamilyProperties); F(vkEnumerateDeviceExtensionProperties);
F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateDevice); F(vkGetDeviceProcAddr); F(vkDestroyDevice); F(vkGetDeviceQueue);
F(vkGetMemoryHostPointerPropertiesEXT); F(vkCreateBuffer); F(vkDestroyBuffer); F(vkGetBufferMemoryRequirements);
F(vkAllocateMemory); F(vkFreeMemory); F(vkBindBufferMemory); F(vkCreateShaderModule); F(vkDestroyShaderModule);
F(vkCreateDescriptorSetLayout); F(vkDestroyDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkDestroyPipelineLayout);
F(vkCreateComputePipelines); F(vkDestroyPipeline); F(vkCreateDescriptorPool); F(vkDestroyDescriptorPool);
F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCreateCommandPool); F(vkDestroyCommandPool);
F(vkAllocateCommandBuffers); F(vkFreeCommandBuffers); F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkCmdBindPipeline);
F(vkCmdBindDescriptorSets); F(vkCmdPushConstants); F(vkCmdDispatch); F(vkCmdPipelineBarrier); F(vkQueueSubmit);
F(vkCreateFence); F(vkDestroyFence); F(vkWaitForFences); F(vkResetFences); F(vkDeviceWaitIdle);
#undef F

#define CHECK(call)                                                          \
    do {                                                                     \
        const VkResult r_ = (call);                                          \
        if (r_ != VK_SUCCESS) {                                              \
            snprintf(why, why_size, "%s: VkResult %d", #call, (int)r_);      \
            return -1;                                                       \
        }                                                                    \
    } while (0)

int vk_probe_init(char *why, size_t why_size, char *info, size_t info_size) {
#define I(name) name = (PFN_##name)vk_icdGetInstanceProcAddr(instance, #name)
    I(vkCreateInstance);
    if (!vkCreateInstance) { snprintf(why, why_size, "vk_icdGetInstanceProcAddr gave no vkCreateInstance"); return -1; }
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "hookshot-plat-probe",
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    I(vkDestroyInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceProperties2);
    I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkEnumerateDeviceExtensionProperties); I(vkGetPhysicalDeviceMemoryProperties);
    I(vkCreateDevice); I(vkGetDeviceProcAddr);
#undef I
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(instance, &n, &gpu);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) { snprintf(why, why_size, "no physical device (%d)", (int)r); return -1; }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host};
    vkGetPhysicalDeviceProperties2(gpu, &props);
    import_alignment = host.minImportedHostPointerAlignment;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &ne, NULL);
    VkExtensionProperties ext[512];
    if (ne > 512) ne = 512;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &ne, ext);
    int has_host = 0;
    for (uint32_t i = 0; i < ne; i++) has_host |= !strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    snprintf(info, info_size, "%s, Vulkan %u.%u, driver %#x; %s; minImportedHostPointerAlignment %llu", props.properties.deviceName,
             VK_API_VERSION_MAJOR(props.properties.apiVersion), VK_API_VERSION_MINOR(props.properties.apiVersion),
             props.properties.driverVersion, has_host ? "VK_EXT_external_memory_host" : "no VK_EXT_external_memory_host",
             (unsigned long long)import_alignment);
    if (!has_host) { snprintf(why, why_size, "the device has no VK_EXT_external_memory_host"); return -1; }
    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, qf);
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && queue_family == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) queue_family = i;
    if (queue_family == UINT32_MAX) { snprintf(why, why_size, "no compute queue"); return -1; }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queue_family,
                                         .queueCount = 1, .pQueuePriorities = &priority};
    const char *const exts[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
    CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkGetMemoryHostPointerPropertiesEXT); D(vkCreateBuffer); D(vkDestroyBuffer);
    D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkFreeMemory); D(vkBindBufferMemory); D(vkCreateShaderModule);
    D(vkDestroyShaderModule); D(vkCreateDescriptorSetLayout); D(vkDestroyDescriptorSetLayout); D(vkCreatePipelineLayout);
    D(vkDestroyPipelineLayout); D(vkCreateComputePipelines); D(vkDestroyPipeline); D(vkCreateDescriptorPool);
    D(vkDestroyDescriptorPool); D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets); D(vkCreateCommandPool);
    D(vkDestroyCommandPool); D(vkAllocateCommandBuffers); D(vkFreeCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets); D(vkCmdPushConstants); D(vkCmdDispatch); D(vkCmdPipelineBarrier);
    D(vkQueueSubmit); D(vkCreateFence); D(vkDestroyFence); D(vkWaitForFences); D(vkResetFences); D(vkDeviceWaitIdle);
#undef D
    if (!vkGetMemoryHostPointerPropertiesEXT) { snprintf(why, why_size, "no vkGetMemoryHostPointerPropertiesEXT"); return -1; }
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    const VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof probe_import_spv,
                                           .pCode = probe_import_spv};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &smci, NULL, &module));
    const VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    const VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
                                                   .pBindings = &b};
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &set_layout));
    const VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8};
    const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                             .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout));
    const VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module, .pName = "main"},
        .layout = pipeline_layout};
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
    vkDestroyShaderModule(device, module, NULL);
    const VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = queue_family};
    CHECK(vkCreateCommandPool(device, &cpi, NULL, &pool));
    const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fci, NULL, &fence));
    return 0;
}

uint64_t vk_probe_import_alignment(void) { return import_alignment; }

/* ptr..+bytes imported, a pattern written by the GPU, checked: the words that differ, or -1 (why says) */
int64_t vk_probe_import(void *ptr, uint64_t bytes, uint32_t seed, void (*between)(void *), void *ctx, double *ms, char *why,
                        size_t why_size) {
    const struct timespec t0 = probe_now();
    uint32_t *const words = (uint32_t *)ptr;
    const uint64_t count = bytes / 4;
    for (uint64_t i = 0; i < count; i++) words[i] = 0xdeadbeefu;   /* (what the GPU must overwrite) */
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(vkGetMemoryHostPointerPropertiesEXT(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp));
    const VkExternalMemoryBufferCreateInfo ebci = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                   .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebci, .size = bytes,
                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const uint32_t types = req.memoryTypeBits & hp.memoryTypeBits;
    if (!types) {
        vkDestroyBuffer(device, buffer, NULL);
        snprintf(why, why_size, "no memory type for the import (buffer %#x, host pointer %#x)", req.memoryTypeBits, hp.memoryTypeBits);
        return -1;
    }
    const VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                  .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                  .pHostPointer = ptr};
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = bytes,
                                      .memoryTypeIndex = (uint32_t)__builtin_ctz(types)};
    VkDeviceMemory memory;
    VkResult r = vkAllocateMemory(device, &mai, NULL, &memory);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, NULL);
        snprintf(why, why_size, "vkAllocateMemory (the import of %llu bytes at %p): VkResult %d", (unsigned long long)bytes, ptr, (int)r);
        return -1;
    }
    int64_t bad = -1;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if ((r = vkBindBufferMemory(device, buffer, memory, 0)) != VK_SUCCESS) { snprintf(why, why_size, "vkBindBufferMemory: %d", (int)r); goto done; }
    if (between) between(ctx);   /* (while the import lives: what the CPU side does to the range meanwhile) */
    const VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    const VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1,
                                             .pPoolSizes = &ps};
    if ((r = vkCreateDescriptorPool(device, &dpci, NULL, &dpool)) != VK_SUCCESS) { snprintf(why, why_size, "descriptor pool: %d", (int)r); goto done; }
    const VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                              .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    if ((r = vkAllocateDescriptorSets(device, &dsai, &set)) != VK_SUCCESS) { snprintf(why, why_size, "descriptor set: %d", (int)r); goto done; }
    const VkDescriptorBufferInfo dbi = {.buffer = buffer, .range = bytes};
    const VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
    const VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if ((r = vkAllocateCommandBuffers(device, &cbai, &cb)) != VK_SUCCESS) { snprintf(why, why_size, "command buffer: %d", (int)r); goto done; }
    const VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, NULL);
    const uint32_t push[2] = {seed, (uint32_t)count};
    vkCmdPushConstants(cb, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdDispatch(cb, 4096, 1, 1);
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vkResetFences(device, 1, &fence);
    if ((r = vkQueueSubmit(queue, 1, &si, fence)) != VK_SUCCESS) { snprintf(why, why_size, "vkQueueSubmit: %d", (int)r); goto done; }
    if ((r = vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000000000ull)) != VK_SUCCESS) {
        snprintf(why, why_size, "vkWaitForFences: %d (%s)", (int)r, r == VK_TIMEOUT ? "the dispatch did not end in 10 s" : "device lost?");
        goto done;
    }
    bad = 0;
    int64_t first = -1;
    for (uint64_t i = 0; i < count; i++)
        if (words[i] != (((uint32_t)i * 2654435761u) ^ seed)) {
            if (first < 0) first = (int64_t)i;
            bad++;
        }
    if (bad) snprintf(why, why_size, "%lld of %llu words wrong, the first at +%#llx (%#x)", (long long)bad, (unsigned long long)count,
                      (unsigned long long)first * 4, words[first]);
done:
    if (cb) vkFreeCommandBuffers(device, pool, 1, &cb);
    if (dpool) vkDestroyDescriptorPool(device, dpool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    *ms = probe_ms_since(t0);
    return bad;
}

void vk_probe_fini(void) {
    if (device) {
        vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
        if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
        vkDestroyDevice(device, NULL);
        device = VK_NULL_HANDLE;
    }
    if (instance) vkDestroyInstance(instance, NULL), instance = VK_NULL_HANDLE;
}
