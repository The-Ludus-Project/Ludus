#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "buffer_range_vulkan.h"

#include <vulkan/vulkan_core.h>

// R13a backend-private observations of native Vulkan synchronization. Recording is opt-in and
// never changes the barriers, admission or resource state it observes; a full trace counts
// dropped records instead of allocating. See docs/architecture/rhi-gdi.md#synchronization-evidence.
namespace ludus::graphics::rhi::backend
{
/// Recording site that emitted a native synchronization operation.
enum class VulkanSynchronizationSite : foundation::uint8
{
    /// Declared use before a live ordered-graph pass.
    GraphPass,
    /// Final or role-state restoration after the last live ordered-graph pass.
    GraphFinal,
    /// Barrier recorded outside ordered-graph lowering in the frame command buffer.
    Direct,
    /// Initial device-local buffer upload in its own transfer submission.
    BufferUpload,
    /// Direct buffer update in its own transfer submission.
    BufferUpdate,
    /// Buffer readback in its own transfer submission.
    BufferReadback,
    /// Texture update, readback or copy in its own transfer submission.
    TextureTransfer,
};
/// Object a recorded native operation covers.
enum class VulkanSynchronizationObject : foundation::uint8
{
    /// RHI buffer; Slot is its backend slot.
    Buffer,
    /// Color image of an RHI texture; Slot is its backend slot.
    ColorImage,
    /// Depth/stencil image of an attachment texture; Slot is its backend slot.
    DepthImage,
    /// Transfer staging buffer; Slot is the transfer slot.
    StagingBuffer,
};
/// One emitted vkCmdPipelineBarrier memory barrier.
struct VulkanSynchronizationRecord final
{
    /// Recording site.
    VulkanSynchronizationSite Site = VulkanSynchronizationSite::Direct;
    /// Covered object.
    VulkanSynchronizationObject Object = VulkanSynchronizationObject::Buffer;
    /// Buffer range decision; Control for images, staging buffers and transfers.
    VulkanBufferRangeOutcome Range = VulkanBufferRangeOutcome::Control;
    /// GPU scope identity (graph pass index plus one) of the following pass; zero when none.
    foundation::uint64 Scope = 0;
    /// Generic frame submission being recorded; zero for transfer submissions and when unavailable.
    foundation::uint64 Submission = 0;
    /// Graph execution identity; zero outside ordered-graph lowering.
    foundation::uint64 Execution = 0;
    /// Graph resource index plus one; zero outside a declared graph use.
    foundation::uint32 GraphResource = 0;
    /// Backend slot of the covered object (see Object).
    foundation::usize Slot = 0;
    /// Emitted source stage mask.
    VkPipelineStageFlags SourceStages = 0;
    /// Emitted destination stage mask.
    VkPipelineStageFlags DestinationStages = 0;
    /// Emitted source access mask.
    VkAccessFlags SourceAccess = 0;
    /// Emitted destination access mask.
    VkAccessFlags DestinationAccess = 0;
    /// Buffer dependency start in bytes; zero for images.
    VkDeviceSize Offset = 0;
    /// Buffer dependency length in bytes or VK_WHOLE_SIZE; zero for images.
    VkDeviceSize Size = 0;
    /// Byte offset of the transfer copy this dependency orders; zero when none.
    VkDeviceSize CopyOffset = 0;
    /// Byte length of the transfer copy this dependency orders; zero when none.
    VkDeviceSize CopySize = 0;
    /// Image layout before the barrier; undefined for buffers.
    VkImageLayout OldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    /// Image layout after the barrier; undefined for buffers.
    VkImageLayout NewLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    /// Image aspects and mip/layer span; empty for buffers.
    VkImageSubresourceRange Subresources{};
};
/// Number of records one trace retains before counting drops.
inline constexpr foundation::usize VULKAN_SYNCHRONIZATION_RECORD_CAPACITY = 256;
/// Bounded, owner-thread observation store.
class VulkanSynchronizationTrace final
{
public:
    /// Start or stop recording; either way discards retained records and the dropped count.
    void SetEnabled(bool isEnabled) noexcept
    {
        mIsEnabled = isEnabled;
        Clear();
    }
    /// Discard retained records and the dropped count without changing whether recording is enabled.
    void Clear() noexcept
    {
        mCount = 0;
        mDropped = 0;
    }
    /// True when sites should build and append records.
    [[nodiscard]] LUDUS_INLINE bool IsEnabled() const noexcept
    {
        return mIsEnabled;
    }
    /// Append one record, or count it as dropped when the store is full. Ignored while disabled.
    void Append(const VulkanSynchronizationRecord& record) noexcept
    {
        if (mIsEnabled == false)
        {
            return;
        }
        if (mCount == VULKAN_SYNCHRONIZATION_RECORD_CAPACITY)
        {
            ++mDropped;
            return;
        }
        mRecords[mCount] = record;
        ++mCount;
    }
    /// Number of retained records.
    [[nodiscard]] LUDUS_INLINE foundation::usize GetCount() const noexcept
    {
        return mCount;
    }
    /// Records that arrived after the store was full.
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetDropped() const noexcept
    {
        return mDropped;
    }
    /// Retained record in emission order.
    /// @pre index < GetCount().
    [[nodiscard]] LUDUS_INLINE const VulkanSynchronizationRecord& GetRecord(foundation::usize index) const noexcept
    {
        return mRecords[index];
    }

private:
    foundation::StaticArray<VulkanSynchronizationRecord, VULKAN_SYNCHRONIZATION_RECORD_CAPACITY> mRecords{};
    foundation::usize mCount = 0;
    foundation::uint64 mDropped = 0;
    bool mIsEnabled = false;
};

// Backend-private controls, defined by the Vulkan backend and called by tests. Owner thread only.
/// Enable or disable observation recording; either way the trace is cleared.
void SetVulkanSynchronizationTrace(bool isEnabled) noexcept;
/// Current observations.
[[nodiscard]] const VulkanSynchronizationTrace& GetVulkanSynchronizationTrace() noexcept;
/// Select the opt-in buffer-range trial (true) or the whole-buffer control (false, the default).
/// Changing the selection discards range snapshots; it takes effect at the next graph barrier.
void SetVulkanBufferRangeTrial(bool isEnabled) noexcept;
} // namespace ludus::graphics::rhi::backend
