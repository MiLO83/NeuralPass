#include <vulkan/vulkan.h>

#include "vulkan_capture_shader_spv.hpp"
#include "vulkan_instrument_test_spv.hpp"
#include "vulkan_spirv.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t width = 64;
constexpr std::uint32_t height = 64;
constexpr std::uint64_t material = 0x1122334455667788ull;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void vk_require(VkResult result, const char *message) {
    if (result != VK_SUCCESS) throw std::runtime_error(
        std::string(message) + " (VkResult " + std::to_string(result) + ")");
}

std::vector<std::uint32_t> shader_words(const unsigned char *bytes,
                                        std::size_t byte_count) {
    require(byte_count % sizeof(std::uint32_t) == 0, "SPIR-V is not word aligned");
    std::vector<std::uint32_t> result(byte_count / sizeof(std::uint32_t));
    std::memcpy(result.data(), bytes, byte_count);
    return result;
}

struct Vulkan {
    HMODULE library = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;

    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
    PFN_vkCreateDevice create_device = nullptr;

    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkGetImageMemoryRequirements get_image_requirements = nullptr;
    PFN_vkCreateImageView create_image_view = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_requirements = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkBindImageMemory bind_image_memory = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkUnmapMemory unmap_memory = nullptr;
    PFN_vkFlushMappedMemoryRanges flush_ranges = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidate_ranges = nullptr;
    PFN_vkCreateRenderPass create_render_pass = nullptr;
    PFN_vkDestroyRenderPass destroy_render_pass = nullptr;
    PFN_vkCreateFramebuffer create_framebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
    PFN_vkCreateShaderModule create_shader_module = nullptr;
    PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
    PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCreateCommandPool create_command_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass = nullptr;
    PFN_vkCmdEndRenderPass cmd_end_render_pass = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
    PFN_vkCmdBindVertexBuffers cmd_bind_vertex_buffers = nullptr;
    PFN_vkCmdDraw cmd_draw = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image = nullptr;
    PFN_vkCmdCopyImage cmd_copy_image = nullptr;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkWaitForFences wait_for_fences = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;

    template <typename T>
    T global(const char *name) const {
        auto value = reinterpret_cast<T>(get_instance_proc(VK_NULL_HANDLE, name));
        require(value != nullptr, "missing Vulkan global function");
        return value;
    }

    template <typename T>
    T instance(VkInstance handle, const char *name) const {
        auto value = reinterpret_cast<T>(get_instance_proc(handle, name));
        require(value != nullptr, "missing Vulkan instance function");
        return value;
    }

    template <typename T>
    T device(VkDevice handle, const char *name) const {
        auto value = reinterpret_cast<T>(get_device_proc(handle, name));
        require(value != nullptr, "missing Vulkan device function");
        return value;
    }

    bool open() {
        library = LoadLibraryW(L"vulkan-1.dll");
        if (library == nullptr) return false;
        get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(library, "vkGetInstanceProcAddr"));
        require(get_instance_proc != nullptr, "vulkan-1.dll has no vkGetInstanceProcAddr");
        create_instance = global<PFN_vkCreateInstance>("vkCreateInstance");
        return true;
    }

    void load_instance(VkInstance handle) {
#define NP_LOAD_INSTANCE(member, name) member = instance<PFN_##name>(handle, #name)
        NP_LOAD_INSTANCE(destroy_instance, vkDestroyInstance);
        NP_LOAD_INSTANCE(enumerate_physical_devices, vkEnumeratePhysicalDevices);
        NP_LOAD_INSTANCE(get_physical_device_properties, vkGetPhysicalDeviceProperties);
        NP_LOAD_INSTANCE(get_queue_properties, vkGetPhysicalDeviceQueueFamilyProperties);
        NP_LOAD_INSTANCE(get_memory_properties, vkGetPhysicalDeviceMemoryProperties);
        NP_LOAD_INSTANCE(create_device, vkCreateDevice);
        NP_LOAD_INSTANCE(get_device_proc, vkGetDeviceProcAddr);
#undef NP_LOAD_INSTANCE
    }

    void load_device(VkDevice handle) {
#define NP_LOAD_DEVICE(member, name) member = device<PFN_##name>(handle, #name)
        NP_LOAD_DEVICE(destroy_device, vkDestroyDevice);
        NP_LOAD_DEVICE(get_device_queue, vkGetDeviceQueue);
        NP_LOAD_DEVICE(create_image, vkCreateImage);
        NP_LOAD_DEVICE(destroy_image, vkDestroyImage);
        NP_LOAD_DEVICE(get_image_requirements, vkGetImageMemoryRequirements);
        NP_LOAD_DEVICE(create_image_view, vkCreateImageView);
        NP_LOAD_DEVICE(destroy_image_view, vkDestroyImageView);
        NP_LOAD_DEVICE(create_buffer, vkCreateBuffer);
        NP_LOAD_DEVICE(destroy_buffer, vkDestroyBuffer);
        NP_LOAD_DEVICE(get_buffer_requirements, vkGetBufferMemoryRequirements);
        NP_LOAD_DEVICE(allocate_memory, vkAllocateMemory);
        NP_LOAD_DEVICE(free_memory, vkFreeMemory);
        NP_LOAD_DEVICE(bind_image_memory, vkBindImageMemory);
        NP_LOAD_DEVICE(bind_buffer_memory, vkBindBufferMemory);
        NP_LOAD_DEVICE(map_memory, vkMapMemory);
        NP_LOAD_DEVICE(unmap_memory, vkUnmapMemory);
        NP_LOAD_DEVICE(flush_ranges, vkFlushMappedMemoryRanges);
        NP_LOAD_DEVICE(invalidate_ranges, vkInvalidateMappedMemoryRanges);
        NP_LOAD_DEVICE(create_render_pass, vkCreateRenderPass);
        NP_LOAD_DEVICE(destroy_render_pass, vkDestroyRenderPass);
        NP_LOAD_DEVICE(create_framebuffer, vkCreateFramebuffer);
        NP_LOAD_DEVICE(destroy_framebuffer, vkDestroyFramebuffer);
        NP_LOAD_DEVICE(create_shader_module, vkCreateShaderModule);
        NP_LOAD_DEVICE(destroy_shader_module, vkDestroyShaderModule);
        NP_LOAD_DEVICE(create_pipeline_layout, vkCreatePipelineLayout);
        NP_LOAD_DEVICE(destroy_pipeline_layout, vkDestroyPipelineLayout);
        NP_LOAD_DEVICE(create_graphics_pipelines, vkCreateGraphicsPipelines);
        NP_LOAD_DEVICE(destroy_pipeline, vkDestroyPipeline);
        NP_LOAD_DEVICE(create_command_pool, vkCreateCommandPool);
        NP_LOAD_DEVICE(destroy_command_pool, vkDestroyCommandPool);
        NP_LOAD_DEVICE(allocate_command_buffers, vkAllocateCommandBuffers);
        NP_LOAD_DEVICE(begin_command_buffer, vkBeginCommandBuffer);
        NP_LOAD_DEVICE(end_command_buffer, vkEndCommandBuffer);
        NP_LOAD_DEVICE(cmd_begin_render_pass, vkCmdBeginRenderPass);
        NP_LOAD_DEVICE(cmd_end_render_pass, vkCmdEndRenderPass);
        NP_LOAD_DEVICE(cmd_bind_pipeline, vkCmdBindPipeline);
        NP_LOAD_DEVICE(cmd_bind_vertex_buffers, vkCmdBindVertexBuffers);
        NP_LOAD_DEVICE(cmd_draw, vkCmdDraw);
        NP_LOAD_DEVICE(cmd_pipeline_barrier, vkCmdPipelineBarrier);
        NP_LOAD_DEVICE(cmd_copy_buffer_to_image, vkCmdCopyBufferToImage);
        NP_LOAD_DEVICE(cmd_copy_image, vkCmdCopyImage);
        NP_LOAD_DEVICE(cmd_copy_image_to_buffer, vkCmdCopyImageToBuffer);
        NP_LOAD_DEVICE(create_fence, vkCreateFence);
        NP_LOAD_DEVICE(destroy_fence, vkDestroyFence);
        NP_LOAD_DEVICE(queue_submit, vkQueueSubmit);
        NP_LOAD_DEVICE(wait_for_fences, vkWaitForFences);
        NP_LOAD_DEVICE(device_wait_idle, vkDeviceWaitIdle);
#undef NP_LOAD_DEVICE
    }

    ~Vulkan() { if (library != nullptr) FreeLibrary(library); }
};

std::uint32_t memory_type(const VkPhysicalDeviceMemoryProperties &properties,
                          std::uint32_t bits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred = 0) {
    std::uint32_t fallback = UINT32_MAX;
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) == 0 ||
            (properties.memoryTypes[index].propertyFlags & required) != required)
            continue;
        if ((properties.memoryTypes[index].propertyFlags & preferred) == preferred)
            return index;
        if (fallback == UINT32_MAX) fallback = index;
    }
    require(fallback != UINT32_MAX, "no compatible Vulkan memory type");
    return fallback;
}

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryPropertyFlags flags = 0;
};

Allocation allocate(Vulkan &vk, VkDevice device,
                    const VkPhysicalDeviceMemoryProperties &properties,
                    const VkMemoryRequirements &requirements,
                    VkMemoryPropertyFlags required,
                    VkMemoryPropertyFlags preferred = 0) {
    const auto type = memory_type(properties, requirements.memoryTypeBits,
                                  required, preferred);
    VkMemoryAllocateInfo info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = type;
    Allocation result;
    result.flags = properties.memoryTypes[type].propertyFlags;
    vk_require(vk.allocate_memory(device, &info, nullptr, &result.memory),
               "vkAllocateMemory failed");
    return result;
}

struct BufferAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation allocation;
    VkDeviceSize size = 0;
};

BufferAllocation create_buffer(Vulkan &vk, VkDevice device,
        const VkPhysicalDeviceMemoryProperties &properties, VkDeviceSize size,
        VkBufferUsageFlags usage) {
    BufferAllocation result;
    result.size = size;
    VkBufferCreateInfo info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_require(vk.create_buffer(device, &info, nullptr, &result.buffer),
               "vkCreateBuffer failed");
    VkMemoryRequirements requirements {};
    vk.get_buffer_requirements(device, result.buffer, &requirements);
    result.allocation = allocate(vk, device, properties, requirements,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_require(vk.bind_buffer_memory(device, result.buffer,
                                     result.allocation.memory, 0),
               "vkBindBufferMemory failed");
    return result;
}

void write_buffer(Vulkan &vk, VkDevice device, const BufferAllocation &buffer,
                  const void *data, std::size_t size) {
    require(size <= buffer.size, "Vulkan upload exceeds its buffer");
    void *mapped = nullptr;
    vk_require(vk.map_memory(device, buffer.allocation.memory, 0, size, 0, &mapped),
               "vkMapMemory for upload failed");
    std::memcpy(mapped, data, size);
    if ((buffer.allocation.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        VkMappedMemoryRange range {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.allocation.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vk_require(vk.flush_ranges(device, 1, &range),
                   "vkFlushMappedMemoryRanges for upload failed");
    }
    vk.unmap_memory(device, buffer.allocation.memory);
}

std::vector<std::byte> read_buffer(Vulkan &vk, VkDevice device,
                                   const BufferAllocation &buffer) {
    void *mapped = nullptr;
    vk_require(vk.map_memory(device, buffer.allocation.memory, 0,
                             buffer.size, 0, &mapped),
               "vkMapMemory for isolation readback failed");
    if ((buffer.allocation.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        VkMappedMemoryRange range {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.allocation.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vk_require(vk.invalidate_ranges(device, 1, &range),
                   "vkInvalidateMappedMemoryRanges for isolation failed");
    }
    std::vector<std::byte> result(static_cast<std::size_t>(buffer.size));
    std::memcpy(result.data(), mapped, result.size());
    vk.unmap_memory(device, buffer.allocation.memory);
    return result;
}

struct ImageAllocation {
    VkImage image = VK_NULL_HANDLE;
    Allocation allocation;
};

ImageAllocation create_transfer_image(Vulkan &vk, VkDevice device,
        const VkPhysicalDeviceMemoryProperties &properties) {
    ImageAllocation result;
    VkImageCreateInfo info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {2, 2, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_require(vk.create_image(device, &info, nullptr, &result.image),
               "vkCreateImage for replacement isolation failed");
    VkMemoryRequirements requirements {};
    vk.get_image_requirements(device, result.image, &requirements);
    result.allocation = allocate(vk, device, properties, requirements, 0,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_require(vk.bind_image_memory(device, result.image,
                                    result.allocation.memory, 0),
               "vkBindImageMemory for replacement isolation failed");
    return result;
}

VkShaderModule shader_module(Vulkan &vk, VkDevice device,
                             std::span<const std::uint32_t> words) {
    VkShaderModuleCreateInfo info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size_bytes();
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vk_require(vk.create_shader_module(device, &info, nullptr, &module),
               "vkCreateShaderModule failed");
    return module;
}

int run() {
    Vulkan vk;
    if (!vk.open()) {
        std::cout << "SKIP: Vulkan loader is not installed\n";
        return 77;
    }

    VkApplicationInfo application {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "NeuralPass Vulkan runtime test";
    application.applicationVersion = 1;
    application.pEngineName = "NeuralPass";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    const auto instance_result = vk.create_instance(&instance_info, nullptr, &instance);
    if (instance_result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        std::cout << "SKIP: Vulkan loader has no compatible installed driver\n";
        return 77;
    }
    vk_require(instance_result, "vkCreateInstance failed");
    vk.load_instance(instance);

    std::uint32_t physical_count = 0;
    vk_require(vk.enumerate_physical_devices(instance, &physical_count, nullptr),
               "vkEnumeratePhysicalDevices failed");
    if (physical_count == 0) {
        vk.destroy_instance(instance, nullptr);
        std::cout << "SKIP: Vulkan loader exposes no physical device\n";
        return 77;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    vk_require(vk.enumerate_physical_devices(instance, &physical_count,
                                              physical_devices.data()),
               "Vulkan physical-device enumeration changed");
    const auto physical = physical_devices.front();
    VkPhysicalDeviceProperties physical_properties {};
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vk.get_physical_device_properties(physical, &physical_properties);
    vk.get_memory_properties(physical, &memory_properties);

    std::uint32_t queue_count = 0;
    vk.get_queue_properties(physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vk.get_queue_properties(physical, &queue_count, queues.data());
    std::uint32_t queue_family = UINT32_MAX;
    for (std::uint32_t index = 0; index < queue_count; ++index)
        if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            queue_family = index; break;
        }
    require(queue_family != UINT32_MAX, "Vulkan device has no graphics queue");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device = VK_NULL_HANDLE;
    vk_require(vk.create_device(physical, &device_info, nullptr, &device),
               "vkCreateDevice failed");
    vk.load_device(device);
    VkQueue queue = VK_NULL_HANDLE;
    vk.get_device_queue(device, queue_family, 0, &queue);

    const std::array<VkFormat, 4> formats {
        VK_FORMAT_R32G32B32A32_UINT,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32_SFLOAT,
    };
    const std::array<VkDeviceSize, 4> byte_sizes {
        width * height * 16ull, width * height * 16ull,
        width * height * 16ull, width * height * 4ull,
    };
    std::array<VkImage, 4> images {};
    std::array<VkImageView, 4> views {};
    std::array<Allocation, 4> image_memory {};
    std::array<VkBuffer, 4> readbacks {};
    std::array<Allocation, 4> readback_memory {};
    for (std::size_t index = 0; index < images.size(); ++index) {
        VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = formats[index];
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vk_require(vk.create_image(device, &image_info, nullptr, &images[index]),
                   "vkCreateImage failed");
        VkMemoryRequirements requirements {};
        vk.get_image_requirements(device, images[index], &requirements);
        image_memory[index] = allocate(vk, device, memory_properties, requirements, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_require(vk.bind_image_memory(device, images[index],
                                        image_memory[index].memory, 0),
                   "vkBindImageMemory failed");
        VkImageViewCreateInfo view_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = images[index];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = formats[index];
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_require(vk.create_image_view(device, &view_info, nullptr, &views[index]),
                   "vkCreateImageView failed");

        VkBufferCreateInfo buffer_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = byte_sizes[index];
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vk_require(vk.create_buffer(device, &buffer_info, nullptr, &readbacks[index]),
                   "vkCreateBuffer failed");
        vk.get_buffer_requirements(device, readbacks[index], &requirements);
        readback_memory[index] = allocate(vk, device, memory_properties, requirements,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vk_require(vk.bind_buffer_memory(device, readbacks[index],
                                         readback_memory[index].memory, 0),
                   "vkBindBufferMemory failed");
    }

    std::array<VkAttachmentDescription, 4> attachments {};
    std::array<VkAttachmentReference, 4> color_references {};
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        attachments[index].format = formats[index];
        attachments[index].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[index].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        color_references[index] = {
            static_cast<std::uint32_t>(index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(color_references.size());
    subpass.pColorAttachments = color_references.data();
    std::array<VkSubpassDependency, 2> dependencies {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo render_pass_info {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    render_pass_info.pDependencies = dependencies.data();
    VkRenderPass render_pass = VK_NULL_HANDLE;
    vk_require(vk.create_render_pass(device, &render_pass_info, nullptr, &render_pass),
               "vkCreateRenderPass failed");
    VkFramebufferCreateInfo framebuffer_info {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebuffer_info.pAttachments = views.data();
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    vk_require(vk.create_framebuffer(device, &framebuffer_info, nullptr, &framebuffer),
               "vkCreateFramebuffer failed");

    auto vertex_words = shader_words(neuralpass_vulkan_instrument_test_spv,
                                     sizeof(neuralpass_vulkan_instrument_test_spv));
    auto instrumented = neuralpass::vulkan_capture::spirv::instrument_vertex_uv(
        vertex_words, 2, "main");
    require(static_cast<bool>(instrumented), "runtime vertex instrumentation failed");
    auto fragment_words = shader_words(neuralpass_vulkan_capture_spv,
                                       sizeof(neuralpass_vulkan_capture_spv));
    fragment_words = neuralpass::vulkan_capture::spirv::patch_unique_location(
        fragment_words, 31, instrumented.varying_location);
    require(!fragment_words.empty(), "runtime fragment varying patch failed");
    const auto vertex_shader = shader_module(vk, device, instrumented.words);
    const auto fragment_shader = shader_module(vk, device, fragment_words);

    VkPipelineLayoutCreateInfo layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    vk_require(vk.create_pipeline_layout(device, &layout_info, nullptr, &pipeline_layout),
               "vkCreatePipelineLayout failed");
    const std::array<VkSpecializationMapEntry, 2> map_entries {{
        {0, 0, sizeof(std::uint32_t)}, {1, sizeof(std::uint32_t), sizeof(std::uint32_t)}}};
    const std::array<std::uint32_t, 2> material_words {
        static_cast<std::uint32_t>(material), static_cast<std::uint32_t>(material >> 32)};
    VkSpecializationInfo specialization {};
    specialization.mapEntryCount = static_cast<std::uint32_t>(map_entries.size());
    specialization.pMapEntries = map_entries.data();
    specialization.dataSize = sizeof(material_words);
    specialization.pData = material_words.data();
    std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &specialization;
    VkVertexInputBindingDescription binding {0, 5 * sizeof(float),
                                              VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> attributes {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float)},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo input_assembly {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    VkRect2D scissor {{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewport_state {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1; viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1; viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterizer {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::array<VkPipelineColorBlendAttachmentState, 4> blend_attachments {};
    for (auto &blend : blend_attachments)
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend_state {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = static_cast<std::uint32_t>(blend_attachments.size());
    blend_state.pAttachments = blend_attachments.data();
    VkGraphicsPipelineCreateInfo pipeline_info {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vk_require(vk.create_graphics_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                             nullptr, &pipeline),
               "vkCreateGraphicsPipelines rejected instrumented capture shaders");

    const std::array<float, 15> vertices {
        -1.0f, -1.0f, 0.25f, 0.0f, 0.0f,
         3.0f, -1.0f, 0.25f, 2.0f, 0.0f,
        -1.0f,  3.0f, 0.25f, 0.0f, 2.0f,
    };
    VkBufferCreateInfo vertex_buffer_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    vertex_buffer_info.size = sizeof(vertices);
    vertex_buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vertex_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    vk_require(vk.create_buffer(device, &vertex_buffer_info, nullptr, &vertex_buffer),
               "vkCreateBuffer for vertices failed");
    VkMemoryRequirements vertex_requirements {};
    vk.get_buffer_requirements(device, vertex_buffer, &vertex_requirements);
    auto vertex_memory = allocate(vk, device, memory_properties, vertex_requirements,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_require(vk.bind_buffer_memory(device, vertex_buffer, vertex_memory.memory, 0),
               "vkBindBufferMemory for vertices failed");
    void *mapped = nullptr;
    vk_require(vk.map_memory(device, vertex_memory.memory, 0, sizeof(vertices), 0, &mapped),
               "vkMapMemory for vertices failed");
    std::memcpy(mapped, vertices.data(), sizeof(vertices));
    if ((vertex_memory.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        VkMappedMemoryRange range {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = vertex_memory.memory; range.offset = 0; range.size = VK_WHOLE_SIZE;
        vk_require(vk.flush_ranges(device, 1, &range), "vkFlushMappedMemoryRanges failed");
    }
    vk.unmap_memory(device, vertex_memory.memory);

    // Exercise the replacement algorithm with real Vulkan resources: clone the
    // application's source image, patch one covered texel in the clone, and prove
    // the source resource and every uncovered replacement texel remain unchanged.
    auto isolation_source = create_transfer_image(vk, device, memory_properties);
    auto isolation_replacement = create_transfer_image(vk, device, memory_properties);
    auto isolation_upload = create_buffer(vk, device, memory_properties, 16,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto isolation_patch = create_buffer(vk, device, memory_properties, 4,
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto isolation_source_readback = create_buffer(vk, device, memory_properties, 16,
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto isolation_replacement_readback = create_buffer(
        vk, device, memory_properties, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const std::array<std::uint8_t, 16> original_texels {
        1, 2, 3, 255, 11, 12, 13, 255,
        21, 22, 23, 255, 31, 32, 33, 255,
    };
    const std::array<std::uint8_t, 4> styled_texel {201, 202, 203, 255};
    write_buffer(vk, device, isolation_upload, original_texels.data(),
                 original_texels.size());
    write_buffer(vk, device, isolation_patch, styled_texel.data(), styled_texel.size());

    VkCommandPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vk_require(vk.create_command_pool(device, &pool_info, nullptr, &pool),
               "vkCreateCommandPool failed");
    VkCommandBufferAllocateInfo command_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    vk_require(vk.allocate_command_buffers(device, &command_info, &commands),
               "vkAllocateCommandBuffers failed");
    VkCommandBufferBeginInfo begin_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vk_require(vk.begin_command_buffer(commands, &begin_info), "vkBeginCommandBuffer failed");
    std::array<VkClearValue, 4> clear_values {};
    VkRenderPassBeginInfo render_begin {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render_begin.renderPass = render_pass;
    render_begin.framebuffer = framebuffer;
    render_begin.renderArea = scissor;
    render_begin.clearValueCount = static_cast<std::uint32_t>(clear_values.size());
    render_begin.pClearValues = clear_values.data();
    vk.cmd_begin_render_pass(commands, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    vk.cmd_bind_pipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize vertex_offset = 0;
    vk.cmd_bind_vertex_buffers(commands, 0, 1, &vertex_buffer, &vertex_offset);
    vk.cmd_draw(commands, 3, 1, 0, 0);
    vk.cmd_end_render_pass(commands);
    for (std::size_t index = 0; index < images.size(); ++index) {
        VkBufferImageCopy copy {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vk.cmd_copy_image_to_buffer(commands, images[index],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbacks[index], 1, &copy);
    }

    std::array<VkImageMemoryBarrier, 2> isolation_begin {};
    for (std::size_t index = 0; index < isolation_begin.size(); ++index) {
        isolation_begin[index] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        isolation_begin[index].srcAccessMask = 0;
        isolation_begin[index].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        isolation_begin[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        isolation_begin[index].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        isolation_begin[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        isolation_begin[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        isolation_begin[index].image = index == 0
            ? isolation_source.image : isolation_replacement.image;
        isolation_begin[index].subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vk.cmd_pipeline_barrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(isolation_begin.size()), isolation_begin.data());
    VkBufferImageCopy full_upload {};
    full_upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    full_upload.imageExtent = {2, 2, 1};
    vk.cmd_copy_buffer_to_image(commands, isolation_upload.buffer,
        isolation_source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full_upload);
    VkImageMemoryBarrier source_ready {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    source_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_ready.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    source_ready.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_ready.image = isolation_source.image;
    source_ready.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.cmd_pipeline_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &source_ready);
    VkImageCopy clone_region {};
    clone_region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    clone_region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    clone_region.extent = {2, 2, 1};
    vk.cmd_copy_image(commands, isolation_source.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, isolation_replacement.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &clone_region);
    VkImageMemoryBarrier clone_ready {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    clone_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clone_ready.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clone_ready.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    clone_ready.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    clone_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clone_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clone_ready.image = isolation_replacement.image;
    clone_ready.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.cmd_pipeline_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &clone_ready);
    VkBufferImageCopy patch_region {};
    patch_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    patch_region.imageOffset = {1, 0, 0};
    patch_region.imageExtent = {1, 1, 1};
    vk.cmd_copy_buffer_to_image(commands, isolation_patch.buffer,
        isolation_replacement.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &patch_region);
    VkImageMemoryBarrier replacement_ready {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    replacement_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    replacement_ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    replacement_ready.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    replacement_ready.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    replacement_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    replacement_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    replacement_ready.image = isolation_replacement.image;
    replacement_ready.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.cmd_pipeline_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        1, &replacement_ready);
    VkBufferImageCopy isolation_readback {};
    isolation_readback.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    isolation_readback.imageExtent = {2, 2, 1};
    vk.cmd_copy_image_to_buffer(commands, isolation_source.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, isolation_source_readback.buffer,
        1, &isolation_readback);
    vk.cmd_copy_image_to_buffer(commands, isolation_replacement.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, isolation_replacement_readback.buffer,
        1, &isolation_readback);
    vk_require(vk.end_command_buffer(commands), "vkEndCommandBuffer failed");
    VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vk_require(vk.create_fence(device, &fence_info, nullptr, &fence),
               "vkCreateFence failed");
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    vk_require(vk.queue_submit(queue, 1, &submit, fence), "vkQueueSubmit failed");
    vk_require(vk.wait_for_fences(device, 1, &fence, VK_TRUE, 10'000'000'000ull),
               "Vulkan capture fence timed out");

    const std::size_t pixel = (height / 2) * width + width / 2;
    std::array<std::vector<std::byte>, 4> output;
    for (std::size_t index = 0; index < readbacks.size(); ++index) {
        vk_require(vk.map_memory(device, readback_memory[index].memory, 0,
                                 byte_sizes[index], 0, &mapped),
                   "vkMapMemory for readback failed");
        if ((readback_memory[index].flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            VkMappedMemoryRange range {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = readback_memory[index].memory;
            range.offset = 0; range.size = VK_WHOLE_SIZE;
            vk_require(vk.invalidate_ranges(device, 1, &range),
                       "vkInvalidateMappedMemoryRanges failed");
        }
        output[index].resize(static_cast<std::size_t>(byte_sizes[index]));
        std::memcpy(output[index].data(), mapped, output[index].size());
        vk.unmap_memory(device, readback_memory[index].memory);
    }
    const auto *surface = reinterpret_cast<const std::uint32_t *>(output[0].data()) + pixel * 4;
    require(surface[0] == static_cast<std::uint32_t>(material) &&
            surface[1] == static_cast<std::uint32_t>(material >> 32),
            "native Vulkan capture returned the wrong material identity");
    float u = 0, v = 0;
    std::memcpy(&u, surface + 2, sizeof(float));
    std::memcpy(&v, surface + 3, sizeof(float));
    require(std::isfinite(u) && std::isfinite(v) &&
            std::abs(u - 0.5f) < 0.05f && std::abs(v - 0.5f) < 0.05f,
            "native Vulkan capture returned the wrong interpolated UV");
    const auto *gradient = reinterpret_cast<const float *>(output[1].data()) + pixel * 4;
    require(std::isfinite(gradient[0]) && std::isfinite(gradient[3]) &&
            gradient[0] > 0.0f && gradient[3] > 0.0f,
            "native Vulkan capture returned invalid UV gradients");
    const auto *source = reinterpret_cast<const float *>(output[2].data()) + pixel * 4;
    require(std::isnan(source[0]), "descriptor-free Vulkan capture did not mark source unknown");
    const auto *depth = reinterpret_cast<const float *>(output[3].data()) + pixel;
    require(std::isfinite(*depth) && std::abs(*depth - 0.25f) < 0.01f,
            "native Vulkan capture returned the wrong fragment depth");
    const auto source_after = read_buffer(vk, device, isolation_source_readback);
    const auto replacement_after = read_buffer(vk, device,
                                               isolation_replacement_readback);
    require(std::memcmp(source_after.data(), original_texels.data(),
                        original_texels.size()) == 0,
            "Vulkan replacement modified the application source image");
    auto expected_replacement = original_texels;
    std::copy(styled_texel.begin(), styled_texel.end(),
              expected_replacement.begin() + 4);
    require(std::memcmp(replacement_after.data(), expected_replacement.data(),
                        expected_replacement.size()) == 0,
            "Vulkan replacement did not isolate its covered texel");

    vk.device_wait_idle(device);
    vk.destroy_fence(device, fence, nullptr);
    vk.destroy_command_pool(device, pool, nullptr);
    vk.destroy_buffer(device, vertex_buffer, nullptr);
    vk.free_memory(device, vertex_memory.memory, nullptr);
    for (const auto *buffer : {&isolation_upload, &isolation_patch,
                              &isolation_source_readback,
                              &isolation_replacement_readback}) {
        vk.destroy_buffer(device, buffer->buffer, nullptr);
        vk.free_memory(device, buffer->allocation.memory, nullptr);
    }
    for (const auto *image : {&isolation_source, &isolation_replacement}) {
        vk.destroy_image(device, image->image, nullptr);
        vk.free_memory(device, image->allocation.memory, nullptr);
    }
    vk.destroy_pipeline(device, pipeline, nullptr);
    vk.destroy_pipeline_layout(device, pipeline_layout, nullptr);
    vk.destroy_shader_module(device, fragment_shader, nullptr);
    vk.destroy_shader_module(device, vertex_shader, nullptr);
    vk.destroy_framebuffer(device, framebuffer, nullptr);
    vk.destroy_render_pass(device, render_pass, nullptr);
    for (std::size_t index = 0; index < images.size(); ++index) {
        vk.destroy_buffer(device, readbacks[index], nullptr);
        vk.free_memory(device, readback_memory[index].memory, nullptr);
        vk.destroy_image_view(device, views[index], nullptr);
        vk.destroy_image(device, images[index], nullptr);
        vk.free_memory(device, image_memory[index].memory, nullptr);
    }
    vk.destroy_device(device, nullptr);
    vk.destroy_instance(instance, nullptr);
    std::cout << "Native Vulkan instrumented capture and replacement isolation passed on "
              << physical_properties.deviceName << '\n';
    return 0;
}

} // namespace

int main() {
    try { return run(); }
    catch (const std::exception &error) {
        std::cerr << "Vulkan runtime test failed: " << error.what() << '\n';
        return 1;
    }
}
