// SPDX-License-Identifier: GPL-2.0-or-later
#if BB_HAS_CMAA2
#include "video_core/renderer_vulkan/vk_cmaa2.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "video_core/host_shaders/cmaa2_spirv.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {
namespace {
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

// The reference uses a 16x16 quad input footprint, 14x14 useful quads per group.
constexpr u32 CeilDiv(u32 a, u32 b) { return (a + b - 1) / b; }
} // namespace

Cmaa2Pass::Cmaa2Pass(const Instance& instance_, Scheduler& scheduler_)
    : instance(instance_), scheduler(scheduler_), device(instance.GetDevice()),
      allocator(instance.GetAllocator()) {
    std::array<vk::DescriptorSetLayoutBinding, 9> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = i == 8 ? vk::DescriptorType::eSampledImage :
                                         (i == 0 || i == 1 || i == 5) ? vk::DescriptorType::eStorageImage :
                                                                     vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    descriptor_layout = Check<"create CMAA2 push descriptor layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data(),
        }));
    pipeline_layout = Check<"create CMAA2 pipeline layout">(
        device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*descriptor_layout,
        }));
    CreatePipelines();
}

Cmaa2Pass::~Cmaa2Pass() {
    scheduler.Finish();
    for (auto& view : views) view.reset();
    for (auto& image : images) image = VideoCore::UniqueImage{};
    DestroyBuffers();
}

void Cmaa2Pass::CreatePipelines() {
    // DXC's default SPIR-V entry point name is the HLSL function passed to -E.
    // The generated modules do not export a generic "main" entry point.
    constexpr std::array<const char*, 4> entry_names{{
        "EdgesColor2x2CS", "ComputeDispatchArgsCS",
        "ProcessCandidatesCS", "DeferredColorApply2x2CS",
    }};
    const std::array<std::array<std::span<const u32>, 4>, 2> spirv{{
        {{CMAA2_HDR_EDGES, CMAA2_HDR_ARGS, CMAA2_HDR_CANDIDATES, CMAA2_HDR_APPLY}},
        {{CMAA2_LDR_EDGES, CMAA2_LDR_ARGS, CMAA2_LDR_CANDIDATES, CMAA2_LDR_APPLY}},
    }};
    for (u32 fmt = 0; fmt < 2; ++fmt) {
        for (u32 i = 0; i < 4; ++i) {
            const auto module = CompileSPV(spirv[fmt][i], device);
            const vk::ComputePipelineCreateInfo ci{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module, .pName = entry_names[i]},
                .layout = *pipeline_layout,
            };
            auto result = device.createComputePipelineUnique({}, ci);
            if (result.result != vk::Result::eSuccess) {
                std::fprintf(stderr, "CMAA2: Vulkan compute pipeline %s/%s failed: %s\n",
                             fmt == 0 ? "HDR" : "LDR", entry_names[i],
                             vk::to_string(result.result).c_str());
            }
            pipelines[fmt][i] = Check<"create CMAA2 compute pipeline">(std::move(result));
            device.destroyShaderModule(module);
        }
    }
}

void Cmaa2Pass::DestroyBuffers() {
    for (auto& b : buffers) {
        if (b.buffer) vmaDestroyBuffer(allocator, static_cast<VkBuffer>(b.buffer), b.allocation);
        b = {};
    }
}

void Cmaa2Pass::Resize(u32 width, u32 height) {
    // A recorded dispatch may still be reading scratch storage from a previous frame.
    scheduler.Finish();
    for (auto& view : views) view.reset();
    for (auto& image : images) image = VideoCore::UniqueImage{};
    DestroyBuffers();
    current_width = width;
    current_height = height;
    const std::array<vk::Format, 2> formats{vk::Format::eR8Uint, vk::Format::eR32Uint};
    const std::array<vk::Extent2D, 2> sizes{{{CeilDiv(width, 2), height},
                                              {CeilDiv(width, 2), CeilDiv(height, 2)}}};
    for (u32 i = 0; i < images.size(); ++i) {
        images[i] = VideoCore::UniqueImage(device, allocator);
        images[i].Create({
            .imageType = vk::ImageType::e2D, .format = formats[i],
            .extent = {sizes[i].width, sizes[i].height, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        views[i] = Check<"create CMAA2 scratch image view">(device.createImageViewUnique({
            .image = vk::Image(images[i]), .viewType = vk::ImageViewType::e2D,
            .format = formats[i], .subresourceRange = ColorRange,
        }));
    }
    // Full-capacity buffers are intentionally larger than Intel's heuristically sized buffers.
    // They avoid candidate-list overflow for dense foliage/noisy scenes.
    const vk::DeviceSize pixels = vk::DeviceSize(width) * height;
    const std::array<vk::DeviceSize, 5> sizes_b{
        pixels * 4, vk::DeviceSize(CeilDiv(width, 2)) * CeilDiv(height, 2) * 4,
        pixels * 8, 64, 16};
    for (u32 i = 0; i < buffers.size(); ++i) {
        const vk::BufferCreateInfo info{
            .size = sizes_b[i],
            .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst |
                     (i == 4 ? vk::BufferUsageFlagBits::eIndirectBuffer : vk::BufferUsageFlags{}),
            .sharingMode = vk::SharingMode::eExclusive,
        };
        const VkBufferCreateInfo ci = static_cast<VkBufferCreateInfo>(info);
        VkBuffer b{};
        VmaAllocation a{};
        const VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO};
        if (vmaCreateBuffer(allocator, &ci, &alloc_info, &b, &a, nullptr) != VK_SUCCESS)
            throw std::runtime_error("CMAA2: failed to allocate scratch buffer");
        buffers[i] = {vk::Buffer{b}, a, sizes_b[i]};
    }
}

void Cmaa2Pass::Prepare(u32 width, u32 height) {
    if (width != current_width || height != current_height) Resize(width, height);
}

bool Cmaa2Pass::Record(vk::CommandBuffer cmd, vk::Image input, vk::ImageView input_view,
                       vk::Image output, vk::ImageView output_view, u32 width, u32 height,
                       vk::Format format) {
    if (!input || !output || input == output || !width || !height ||
        (format != vk::Format::eR16G16B16A16Sfloat && format != vk::Format::eR8G8B8A8Unorm))
        return false;
    if (width != current_width || height != current_height) return false;
    // CMAA2 only overwrites detected edges. Copy the rest, preserving original alpha.
    const vk::ImageCopy copy{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {width, height, 1},
    };
    const vk::MemoryBarrier2 before_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before_copy});
    cmd.copyImage(input, vk::ImageLayout::eGeneral, output, vk::ImageLayout::eGeneral, copy);
    cmd.fillBuffer(buffers[3].buffer, 0, buffers[3].bytes, 0);
    cmd.fillBuffer(buffers[4].buffer, 0, buffers[4].bytes, 0);
    for (auto& image : images) {
        const vk::ImageMemoryBarrier2 init{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(image), .subresourceRange = ColorRange,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &init});
    }
    // The scratch images have integer formats. With VULKAN_HPP_NO_CONSTRUCTORS,
    // ClearColorValue is a union whose first member is float32, so initializing
    // it with a uint32 std::array incorrectly tries to initialize float32.
    vk::ClearColorValue heads_clear{};
    heads_clear.uint32[0] = 0xffffffffu;
    cmd.clearColorImage(vk::Image(images[1]), vk::ImageLayout::eGeneral,
                        heads_clear, ColorRange);
    // The shader initializes every edge texel. Clear too, to be safe at odd sizes.
    vk::ClearColorValue edges_clear{};
    cmd.clearColorImage(vk::Image(images[0]), vk::ImageLayout::eGeneral,
                        edges_clear, ColorRange);

    const std::array<vk::DescriptorImageInfo, 4> img{{
        {.imageView = output_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *views[0], .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *views[1], .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = input_view, .imageLayout = vk::ImageLayout::eGeneral},
    }};
    std::array<vk::DescriptorBufferInfo, 5> buf{};
    for (u32 i = 0; i < buf.size(); ++i)
        buf[i] = {.buffer = buffers[i].buffer, .offset = 0, .range = buffers[i].bytes};
    std::array<vk::WriteDescriptorSet, 9> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        const bool image = i == 0 || i == 1 || i == 5 || i == 8;
        const u32 image_idx = i == 0 ? 0 : i == 1 ? 1 : i == 5 ? 2 : 3;
        const u32 buffer_idx = i == 2 ? 0 : i == 3 ? 1 : i == 4 ? 2 : i == 6 ? 3 : 4;
        writes[i] = {.dstBinding = i, .descriptorCount = 1,
                     .descriptorType = image ? (i == 8 ? vk::DescriptorType::eSampledImage :
                                                vk::DescriptorType::eStorageImage) :
                                               vk::DescriptorType::eStorageBuffer,
                     .pImageInfo = image ? &img[image_idx] : nullptr,
                     .pBufferInfo = image ? nullptr : &buf[buffer_idx]};
    }
    const vk::MemoryBarrier2 read_after_transfer{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &read_after_transfer});
    const u32 mode = format == vk::Format::eR16G16B16A16Sfloat ? 0 : 1;
    const auto bind = [&](u32 pass) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[mode][pass]);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    };
    const auto sync = [&] {
        const vk::MemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                            vk::PipelineStageFlagBits2::eDrawIndirect,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                             vk::AccessFlagBits2::eShaderWrite |
                             vk::AccessFlagBits2::eIndirectCommandRead,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
    };
    bind(0); cmd.dispatch(CeilDiv(width, 28), CeilDiv(height, 28), 1);
    sync();
    bind(1); cmd.dispatch(2, 1, 1);
    sync();
    bind(2); cmd.dispatchIndirect(buffers[4].buffer, 0);
    sync();
    bind(1); cmd.dispatch(1, 2, 1);
    sync();
    bind(3); cmd.dispatchIndirect(buffers[4].buffer, 0);
    const vk::MemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    return true;
}
} // namespace Vulkan
#endif // BB_HAS_CMAA2
