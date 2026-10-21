// SPDX-License-Identifier: GPL-2.0-or-later
// Intel CMAA2 single-sample, spatial AA path for Vulkan. HUD is composed afterwards.
#pragma once

#include <array>
#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
class Scheduler;

class Cmaa2Pass {
public:
    Cmaa2Pass(const Instance& instance, Scheduler& scheduler);
    ~Cmaa2Pass();
    Cmaa2Pass(const Cmaa2Pass&) = delete;
    Cmaa2Pass& operator=(const Cmaa2Pass&) = delete;

    // Call before obtaining/recording the current command buffer: resize waits for the GPU.
    void Prepare(u32 width, u32 height);

    // Source and destination are distinct GENERAL-layout images of matching format.
    // Source must support TRANSFER_SRC; destination must support TRANSFER_DST and STORAGE.
    // Record copies unmodified pixels, then executes CMAA2's four real compute entry points.
    bool Record(vk::CommandBuffer cmdbuf, vk::Image input, vk::ImageView input_view,
                vk::Image output, vk::ImageView output_view,
                u32 width, u32 height, vk::Format format);

private:
    struct Buffer {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        vk::DeviceSize bytes{};
    };
    void DestroyBuffers();
    void Resize(u32 width, u32 height);
    void CreatePipelines();

    const Instance& instance;
    Scheduler& scheduler;
    vk::Device device;
    VmaAllocator allocator;
    vk::UniqueDescriptorSetLayout descriptor_layout;
    vk::UniquePipelineLayout pipeline_layout;
    std::array<std::array<vk::UniquePipeline, 4>, 2> pipelines;
    // Edges R8_UINT, linked-list heads R32_UINT.
    std::array<VideoCore::UniqueImage, 2> images;
    std::array<vk::UniqueImageView, 2> views;
    // Shape candidates, deferred blend locations, blend items, counters and indirect args.
    std::array<Buffer, 5> buffers{};
    u32 current_width = 0, current_height = 0;
    bool reported_dispatch = false;
};
} // namespace Vulkan
