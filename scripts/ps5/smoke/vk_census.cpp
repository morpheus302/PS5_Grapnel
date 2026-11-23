// Hookshot on the PS5: the Vulkan back end's requirements against one device (vk_census.h).
//
// Levels, from what gpu/vk does without the item:
//   need   it stops (vk_main.cpp returns false), or the translated programs assume it (vk_shader.cpp make_profile and
//          host_info set it unconditionally), or the game draws wrong without it (subgroups pinned to 32 lanes: issues
//          10 and 14), or the port's design rests on it (guest memory imported in place, mesh shaders)
//   use    used when present; a slower or lesser path otherwise
//   diag   diagnostics only (lost-device reports, markers)
//   info   a value to know (limits, alignment, memory)
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_census.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void (*out)(const char*);
CensusCounts counts;

void line(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    out(b);
}

enum Level { NEED, USE, DIAG, INFO };
const char* level_name(Level l) { return l == NEED ? "need" : l == USE ? "use " : l == DIAG ? "diag" : "info"; }

void check(Level l, bool ok, const char* what, const char* detail = "") {
    if (ok) counts.pass++;
    else if (l == NEED) counts.missing_needed++;
    else if (l == USE) counts.missing_used++;
    else if (l == DIAG) counts.missing_diag++;
    else counts.info++;
    line("%s %s %s%s%s", ok ? "PASS   " : "MISSING", level_name(l), what, *detail ? ": " : "", detail);
}

bool has(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

// (the formats vk_formats.cpp maps NVN's to, collected from its source at build time: scripts/ps5/build-gpu.sh)
struct FormatName {
    VkFormat f;
    const char* name;
};
const FormatName kFormats[] = {
#define X(f) {f, #f},
#include "hookshot_formats.inc"
#undef X
};

bool is_depth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_S8_UINT;
}
bool is_compressed(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}
bool is_astc(VkFormat f) { return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }
bool widened(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SNORM: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT:
    case VK_FORMAT_R16G16B16_SFLOAT: case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SNORM:
    case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return true;
    default: return false;
    }
}

}  // namespace

CensusCounts vk_census(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice pd, void (*say)(const char*)) {
    out = say;
    counts = {};
#define LOAD(f) auto f = (PFN_##f)gipa(instance, #f)
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceFormatProperties);
#undef LOAD
    auto enum_inst_ext = (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
    if (!vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceFeatures2 || !vkEnumerateDeviceExtensionProperties) {
        line("census: the 1.1 queries are missing");
        counts.missing_needed++;
        return counts;
    }

    line("== census: what Hookshot's Vulkan back end (gpu/vk) asks of this device");
    // ---- instance ----
    {
        uint32_t n = 0;
        enum_inst_ext(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> e(n);
        enum_inst_ext(nullptr, &n, e.data());
        check(NEED, has(e, VK_KHR_SURFACE_EXTENSION_NAME), "instance VK_KHR_surface");
#if defined(__PROSPERO__)
        check(NEED, has(e, VK_KHR_DISPLAY_EXTENSION_NAME), "instance VK_KHR_display", "the console's display (vk_ps5.cpp)");
#else
        check(INFO, has(e, VK_KHR_DISPLAY_EXTENSION_NAME), "instance VK_KHR_display", "the console's surface");
#endif
    }

    // ---- properties ----
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDevicePushDescriptorPropertiesKHR pd_p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, ext.data());
    const bool e_mesh = has(ext, VK_EXT_MESH_SHADER_EXTENSION_NAME), e_host = has(ext, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME),
               e_push = has(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),
               e_gpl = has(ext, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has(ext, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    p2.pNext = &sg, sg.pNext = &sc, sc.pNext = &fc, fc.pNext = &drv;
    void** tail = &drv.pNext;
    if (e_mesh) *tail = &mesh_p, tail = &mesh_p.pNext;
    if (e_push) *tail = &pd_p, tail = &pd_p.pNext;
    if (e_host) *tail = &hp, tail = &hp.pNext;
    if (e_gpl) *tail = &gpl_p, tail = &gpl_p.pNext;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const VkPhysicalDeviceProperties& p = p2.properties;
    line("device: %s, Vulkan %u.%u.%u, driver %s %s, %u device extensions", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
         VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), drv.driverName, drv.driverInfo, n);
    check(NEED, p.apiVersion >= VK_API_VERSION_1_3, "Vulkan 1.3", "vk_main.cpp pick_device");

    // ---- extensions, as create_device asks for them ----
    struct E { Level l; const char* name; const char* why; };
    const E exts[] = {
        {NEED, VK_KHR_SWAPCHAIN_EXTENSION_NAME, "always enabled: vkCreateDevice fails without it"},
        {NEED, VK_EXT_MESH_SHADER_EXTENSION_NAME, "NVN2 task/mesh programs: foliage, grass, effects (a compute fallback exists, issue 640)"},
        {NEED, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, "guest memory pools imported in place (vk_memory.cpp)"},
        {USE, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, "descriptors pushed when they fit"},
        {USE, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, "null descriptors, robust buffer access 2"},
        {USE, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, "polygon mode, depth clamp, blend and write masks as dynamic state"},
        {USE, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, "NVN border colours"},
        {USE, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, "NVN's -1..1 depth"},
        {USE, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, "8-bit indices"},
        {USE, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, "RENDER_COND"},
        {USE, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, "last-vertex provoking"},
        {USE, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, "instance divisors"},
        {USE, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, "stencil writes from fragment programs"},
        {USE, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, "the image cache's budget"},
        {USE, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME, "2D samples of 3D textures"},
        {USE, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, "pipeline libraries"},
        {USE, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, "fast-linked pipelines"},
        {USE, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, "native HMMA for the game's networks (emulated otherwise)"},
        {DIAG, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, "checkpoints (on by default where present; NVIDIA's)"},
        {DIAG, VK_AMD_BUFFER_MARKER_EXTENSION_NAME, "RX_VK_CHECKPOINTS=1 markers"},
        {DIAG, VK_EXT_DEVICE_FAULT_EXTENSION_NAME, "what a lost device faulted on"},
        {INFO, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, "Linux only: dma-buf pools (compiled out on the PS5)"},
        {INFO, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, "Linux only"},
    };
    for (const E& e : exts) check(e.l, has(ext, e.name), e.name, e.why);

    // ---- features ----
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT r2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vdiv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT v2d{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT};
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
    tail = &f13.pNext;
    const auto add = [&](bool present, void* s, void** next) {
        if (present) *tail = s, tail = next;
    };
    add(e_mesh, &mesh, &mesh.pNext);
    add(has(ext, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME), &r2, &r2.pNext);
    add(has(ext, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME), &eds3, &eds3.pNext);
    add(has(ext, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME), &border, &border.pNext);
    add(has(ext, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME), &clip, &clip.pNext);
    add(has(ext, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME), &cond, &cond.pNext);
    add(has(ext, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME), &pv, &pv.pNext);
    add(has(ext, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME), &vdiv, &vdiv.pNext);
    add(has(ext, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME), &u8, &u8.pNext);
    add(e_gpl, &gpl, &gpl.pNext);
    add(has(ext, VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME), &v2d, &v2d.pNext);
    add(has(ext, VK_EXT_DEVICE_FAULT_EXTENSION_NAME), &fault, &fault.pNext);
    vkGetPhysicalDeviceFeatures2(pd, &f);
    const VkPhysicalDeviceFeatures& c = f.features;
#define F(level, expr, why) check(level, (expr) != VK_FALSE, #expr, why)
    line("-- features: what vk_main.cpp requires or reads");
    F(NEED, f13.dynamicRendering, "vk_main.cpp: no device without it");
    F(NEED, f13.synchronization2, "vk_main.cpp: no device without it");
    F(NEED, f12.timelineSemaphore, "vk_main.cpp: the NVN fences");
    F(NEED, mesh.meshShader, "dev.mesh_shader");
    F(NEED, mesh.taskShader, "dev.mesh_shader");
    F(NEED, f13.subgroupSizeControl, "programs pinned to 32 lanes");
    F(USE, f13.computeFullSubgroups, "full 32-lane compute subgroups when the workgroup allows (vk_draw.cpp)");
    F(USE, f12.bufferDeviceAddress, "the issue 640 mesh fallback; device addresses");
    F(USE, c.robustBufferAccess, "robust buffers (on by default)");
    F(USE, r2.robustBufferAccess2, "robust buffers");
    F(USE, r2.nullDescriptor, "dev.null_descriptor");
    F(USE, c.sparseBinding, "sparse buffers");
    F(USE, c.sparseResidencyImage2D, "sparse 2D images: the terrain's arrays");
    F(USE, c.samplerAnisotropy, "anisotropic samplers");
    F(USE, c.textureCompressionBC, "BC textures as they are, and ASTC re-encoded to BC (vk_texture.cpp)");
    F(USE, f12.vulkanMemoryModel, "cooperative matrices");
    F(USE, f12.vulkanMemoryModelDeviceScope, "cooperative matrices");
    F(USE, eds3.extendedDynamicState3PolygonMode, "dev.eds3_polygon");
    F(USE, eds3.extendedDynamicState3DepthClampEnable, "dev.eds3_clamp");
    F(USE, eds3.extendedDynamicState3ColorBlendEnable, "dev.eds3_blend");
    F(USE, eds3.extendedDynamicState3ColorBlendEquation, "dev.eds3_blend");
    F(USE, eds3.extendedDynamicState3ColorWriteMask, "dev.eds3_blend");
    F(USE, eds3.extendedDynamicState3AlphaToCoverageEnable, "dev.eds3_a2c");
    F(USE, eds3.extendedDynamicState3DepthClipEnable, "vkCmdSetDepthClipEnableEXT");
    F(USE, eds3.extendedDynamicState3LogicOpEnable, "vkCmdSetLogicOpEnableEXT");
    F(USE, border.customBorderColors, "dev.custom_border_color");
    F(USE, border.customBorderColorWithoutFormat, "dev.custom_border_color");
    F(USE, clip.depthClipControl, "dev.depth_clip_control");
    F(USE, cond.conditionalRendering, "dev.conditional_rendering");
    F(USE, pv.provokingVertexLast, "dev.provoking_vertex");
    F(USE, vdiv.vertexAttributeInstanceRateDivisor, "dev.vertex_divisor");
    F(USE, u8.indexTypeUint8, "dev.index_uint8");
    F(USE, gpl.graphicsPipelineLibrary, "dev.graphics_pipeline_library");
    F(USE, gpl_p.graphicsPipelineLibraryFastLinking, "dev.graphics_pipeline_library");
    F(USE, v2d.image2DViewOf3D, "dev.view_2d_of_3d");
    F(USE, v2d.sampler2DViewOf3D, "dev.view_2d_of_3d");
    F(DIAG, fault.deviceFault, "dev.device_fault");
    line("-- features the translated programs assume (vk_shader.cpp make_profile, host_info: set unconditionally)");
    F(NEED, f12.shaderInt8, "support_int8");
    F(NEED, c.shaderInt16, "support_int16");
    F(NEED, c.shaderInt64, "support_int64");
    F(NEED, c.shaderFloat64, "support_float64");
    F(NEED, f12.shaderFloat16, "support_float16 (RX_VK_NO_F16 lowers it)");
    F(NEED, f12.storageBuffer8BitAccess, "support_storage_buffer_8bit");
    F(NEED, f12.uniformAndStorageBuffer8BitAccess, "support_uniform_and_storage_buffer_8bit");
    F(NEED, f11.storageBuffer16BitAccess, "support_storage_buffer_16bit");
    F(NEED, f11.uniformAndStorageBuffer16BitAccess, "support_uniform_and_storage_buffer_16bit");
    F(NEED, f12.shaderBufferInt64Atomics, "support_int64_atomics");
    F(NEED, f12.shaderSharedInt64Atomics, "support_int64_atomics");
    F(NEED, f13.shaderDemoteToHelperInvocation, "support_demote_to_helper_invocation");
    F(NEED, c.shaderStorageImageReadWithoutFormat, "support_typeless_image_loads");
    F(NEED, c.shaderStorageImageWriteWithoutFormat, "storage images without a format");
    F(NEED, c.multiViewport, "support_multi_viewport");
    F(NEED, f12.shaderOutputViewportIndex, "support_viewport_index_layer_non_geometry");
    F(NEED, f12.shaderOutputLayer, "support_viewport_index_layer_non_geometry");
    F(NEED, f12.shaderSampledImageArrayNonUniformIndexing, "support_sampled_image_array_nonuniform_indexing");
    F(NEED, f12.uniformBufferStandardLayout, "the game's uniform blocks (phase 2)");
    F(USE, f12.scalarBlockLayout, "scalar layouts");
    F(USE, f12.runtimeDescriptorArray, "descriptor arrays");
    F(USE, f12.samplerFilterMinmax, "min/max samplers");
    F(USE, f12.drawIndirectCount, "indirect count draws");
    F(USE, f12.hostQueryReset, "query pools");
    F(USE, f11.shaderDrawParameters, "draw parameters");
    F(USE, c.geometryShader, "geometry programs");
    F(USE, c.tessellationShader, "tessellation programs");
    F(USE, c.independentBlend, "per-target blend");
    F(USE, c.dualSrcBlend, "dual-source blend");
    F(USE, c.logicOp, "logic ops");
    F(USE, c.depthClamp, "depth clamp");
    F(USE, c.depthBiasClamp, "depth bias clamp");
    F(USE, c.depthBounds, "depth bounds");
    F(USE, c.fillModeNonSolid, "polygon modes");
    F(USE, c.wideLines, "line widths");
    F(USE, c.multiDrawIndirect, "indirect draws");
    F(USE, c.drawIndirectFirstInstance, "indirect draws");
    F(USE, c.imageCubeArray, "cube arrays");
    F(USE, c.shaderClipDistance, "clip distances");
    F(USE, c.shaderCullDistance, "cull distances");
    F(USE, c.shaderImageGatherExtended, "gather offsets");
    F(USE, c.sampleRateShading, "sample shading");
    F(USE, c.fragmentStoresAndAtomics, "fragment stores");
    F(USE, c.vertexPipelineStoresAndAtomics, "vertex stores (the mesh fallback)");
    F(USE, c.occlusionQueryPrecise, "occlusion queries");
#undef F

    // ---- properties ----
    line("-- properties");
    const VkShaderStageFlags pin = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_COMPUTE_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT;
    char d[256];
    snprintf(d, sizeof d, "%u-%u lanes (default %u), required-size stages %#x", sc.minSubgroupSize, sc.maxSubgroupSize,
             sg.subgroupSize, (unsigned)sc.requiredSubgroupSizeStages);
    check(NEED, sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32, "32-lane subgroups", d);
    snprintf(d, sizeof d, "stages %#x, need %#x (task, mesh, compute, fragment)", (unsigned)sc.requiredSubgroupSizeStages, (unsigned)pin);
    check(NEED, (sc.requiredSubgroupSizeStages & pin) == pin, "subgroup size pinned where the game's programs run", d);
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                       VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
                                       VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
    snprintf(d, sizeof d, "operations %#x, need %#x", (unsigned)sg.supportedOperations, (unsigned)ops);
    check(NEED, (sg.supportedOperations & ops) == ops, "subgroup operations (votes, ballots, shuffles, quads)", d);
    snprintf(d, sizeof d, "stages %#x", (unsigned)sg.supportedStages);
    check(NEED, (sg.supportedStages & pin) == pin, "subgroup operations in task, mesh, compute and fragment stages", d);
    const auto indep = [](VkShaderFloatControlsIndependence v) {
        return v == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL ? "all" : v == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY
                                                                         ? "32-bit only" : "none";
    };
    snprintf(d, sizeof d, "fp32 denorm preserve %u flush %u, signed zero/inf/nan fp16 %u fp32 %u", fc.shaderDenormPreserveFloat32,
             fc.shaderDenormFlushToZeroFloat32, fc.shaderSignedZeroInfNanPreserveFloat16, fc.shaderSignedZeroInfNanPreserveFloat32);
    check(NEED, fc.shaderDenormPreserveFloat32 && fc.shaderDenormFlushToZeroFloat32 && fc.shaderSignedZeroInfNanPreserveFloat16 &&
                    fc.shaderSignedZeroInfNanPreserveFloat32,
          "float controls make_profile sets", d);
    // (make_profile says support_separate_denorm_behavior and support_separate_rounding_mode, as for INDEPENDENCE_ALL, which
    // NVIDIA reports and desktop RADV does not. Harmless: it leaves support_fp16_denorm_flush / _preserve off, so Eden's
    // SetupDenormControl emits fp32 modes only, which 32-bit-only independence allows)
    line("info    float controls: denorm behaviour %s, rounding mode %s (fp32 modes only are emitted: either is enough)",
         indep(fc.denormBehaviorIndependence), indep(fc.roundingModeIndependence));
    if (e_mesh) {
        // (the game's largest, PLAN.md phase 2: 160 outputs per mesh workgroup, 3,200-byte task payloads, ~21 KiB of mesh
        // shared memory, 32-invocation workgroups)
        snprintf(d, sizeof d, "vertices %u, primitives %u (game: 160)", mesh_p.maxMeshOutputVertices, mesh_p.maxMeshOutputPrimitives);
        check(NEED, mesh_p.maxMeshOutputVertices >= 160 && mesh_p.maxMeshOutputPrimitives >= 160, "mesh outputs", d);
        snprintf(d, sizeof d, "%u bytes (game: 3,200)", mesh_p.maxTaskPayloadSize);
        check(NEED, mesh_p.maxTaskPayloadSize >= 3200, "task payload", d);
        snprintf(d, sizeof d, "%u bytes (game: about 21 KiB)", mesh_p.maxMeshSharedMemorySize);
        check(NEED, mesh_p.maxMeshSharedMemorySize >= 22 * 1024, "mesh shared memory", d);
        snprintf(d, sizeof d, "task %u, mesh %u (game: 32)", mesh_p.maxTaskWorkGroupInvocations, mesh_p.maxMeshWorkGroupInvocations);
        check(NEED, mesh_p.maxTaskWorkGroupInvocations >= 32 && mesh_p.maxMeshWorkGroupInvocations >= 32, "workgroup invocations", d);
        line("info    mesh: max task workgroups %u x %u x %u (total %u), max mesh workgroups %u x %u x %u, payload+shared %u, output memory %u",
             mesh_p.maxTaskWorkGroupCount[0], mesh_p.maxTaskWorkGroupCount[1], mesh_p.maxTaskWorkGroupCount[2],
             mesh_p.maxTaskWorkGroupTotalCount, mesh_p.maxMeshWorkGroupCount[0], mesh_p.maxMeshWorkGroupCount[1],
             mesh_p.maxMeshWorkGroupCount[2], mesh_p.maxMeshPayloadAndSharedMemorySize, mesh_p.maxMeshOutputMemorySize);
    }
    snprintf(d, sizeof d, "%u bytes (NVN constant banks are 64 KiB)", p.limits.maxUniformBufferRange);
    check(NEED, p.limits.maxUniformBufferRange >= 65536, "uniform buffer range", d);
    if (e_host) line("info    host pointer imports: alignment %llu bytes", (unsigned long long)hp.minImportedHostPointerAlignment);
    if (e_push) line("info    push descriptors: at most %u", pd_p.maxPushDescriptors);
    line("info    limits: storage range %u, ssbo align %llu, ubo align %llu, texel align %llu, texel elements %u, anisotropy %.0f, "
         "lod bias %.1f, timestamp period %.2f ns, bound sets %u, sampled images per stage %u",
         p.limits.maxStorageBufferRange, (unsigned long long)p.limits.minStorageBufferOffsetAlignment,
         (unsigned long long)p.limits.minUniformBufferOffsetAlignment, (unsigned long long)p.limits.minTexelBufferOffsetAlignment,
         p.limits.maxTexelBufferElements, p.limits.maxSamplerAnisotropy, p.limits.maxSamplerLodBias, p.limits.timestampPeriod,
         p.limits.maxBoundDescriptorSets, p.limits.maxPerStageDescriptorSampledImages);

    // ---- the queue ----
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf.data());
    int family = -1;
    for (uint32_t i = 0; i < n; i++) {
        line("info    queue family %u: flags %#x, %u queues, timestamp bits %u", i, (unsigned)qf[i].queueFlags, qf[i].queueCount,
             qf[i].timestampValidBits);
        if (family < 0 && (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            family = (int)i;
    }
    check(NEED, family >= 0, "a queue family with graphics and compute");
    check(USE, family >= 0 && (qf[family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT), "sparse binding on that family",
          "dev.sparse_images / sparse_buffers");

    // ---- memory ----
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        line("info    memory heap %u: %llu MiB, flags %#x", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
             (unsigned)mp.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        line("info    memory type %u: heap %u, flags %#x", i, mp.memoryTypes[i].heapIndex, (unsigned)mp.memoryTypes[i].propertyFlags);

    // ---- formats ----
    line("-- formats: every Vulkan format vk_formats.cpp maps an NVN format to (%zu), optimal tiling",
         sizeof kFormats / sizeof kFormats[0]);
    for (const FormatName& fn : kFormats) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(pd, fn.f, &fp);
        const VkFormatFeatureFlags o = fp.optimalTilingFeatures;
        const bool sampled = o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (is_astc(fn.f)) {   // (decoded on the GPU by Eden's astc_decoder.comp when not sampled: vk_formats.cpp astc_native)
            if (fn.f == VK_FORMAT_ASTC_4x4_UNORM_BLOCK)
                line("info    ASTC: %s", sampled ? "sampled natively" : "not sampled: decoded to RGBA8 on the GPU, as on NVIDIA");
            continue;
        }
        char what[96];
        if (widened(fn.f)) {   // (vk_texture.cpp widened(): the image holds 4 components, the upload widens them)
            line("info    %s is %s: textures of it are widened to 4 components (vk_texture.cpp)", fn.name,
                 sampled ? "sampled" : "not an image format here");
            continue;
        }
        if (is_depth(fn.f)) {
            // (D24 is AMD's gap on every RADV: vk_formats.cpp has no substitute for NVN's DEPTH24 / DEPTH24_STENCIL8, so it is
            // needed only if the game uses them. All 1,300 of its task and mesh pipelines use D32_SFLOAT (the phase 2 pack))
            const bool d24 = fn.f == VK_FORMAT_D24_UNORM_S8_UINT || fn.f == VK_FORMAT_X8_D24_UNORM_PACK32;
            snprintf(what, sizeof what, "%s as a depth/stencil target", fn.name);
            check(d24 ? USE : NEED, (o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && sampled, what,
                  (o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                      ? ""
                      : "no substitute in vk_formats.cpp: needed only if the game uses NVN's DEPTH24 or DEPTH24_STENCIL8 "
                        "(its mesh pipelines use D32_SFLOAT)");
        } else if (is_compressed(fn.f)) {
            snprintf(what, sizeof what, "%s sampled", fn.name);
            check(NEED, sampled, what);
        } else {
            snprintf(what, sizeof what, "%s sampled", fn.name);
            check(NEED, sampled, what);
            if (!(o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
                line("info    %s is not a colour target (only matters if the game renders to it)", fn.name);
        }
    }
    line("== census: %d pass; missing: %d needed, %d used-if-present, %d diagnostics", counts.pass, counts.missing_needed,
         counts.missing_used, counts.missing_diag);
    return counts;
}
