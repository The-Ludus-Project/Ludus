#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "buffer_access_vulkan.h"

#include <vulkan/vulkan_core.h>

// R13a buffer-range synchronization trial. The whole-buffer plan is the control and the
// default; the range ledger is opt-in. See docs/architecture/rhi-gdi.md#synchronization-evidence.
namespace ludus::graphics::rhi::backend
{
/// Prior accesses of a buffer or byte range since its last ordering write.
struct VulkanBufferState final
{
    /// Stages of the writer and of every later reader.
    VkPipelineStageFlags Stages = VK_PIPELINE_STAGE_HOST_BIT;
    /// Accesses of the writer and of every later reader.
    VkAccessFlags Access = VK_ACCESS_HOST_WRITE_BIT;
    /// Stages of the last write, retained across later readers.
    VkPipelineStageFlags WriterStages = VK_PIPELINE_STAGE_HOST_BIT;
    /// Access of the last write, retained across later readers.
    VkAccessFlags WriterAccess = VK_ACCESS_HOST_WRITE_BIT;
};
/// Why a barrier covers the byte range or the whole buffer.
enum class VulkanBufferRangeOutcome : foundation::uint8
{
    /// Control path: whole-buffer dependency with whole-buffer prior accesses.
    Control,
    /// The use matched or opened a disjoint range; source scope comes from that range.
    Narrowed,
    /// The use declares no byte range (final role restoration or a direct call).
    WholeUse,
    /// The use partly overlaps a tracked range of the same buffer.
    PartialOverlap,
    /// The ledger has no room for another buffer or range.
    Capacity,
};
/// Legacy Vulkan 1.1 buffer barrier selected for one use.
struct VulkanBufferBarrierPlan final
{
    /// Combined source stages: the retained writer and every later reader.
    VkPipelineStageFlags SourceStages = 0;
    /// Stages of the use.
    VkPipelineStageFlags DestinationStages = 0;
    /// Combined source accesses.
    VkAccessFlags SourceAccess = 0;
    /// Read and write accesses of the use.
    VkAccessFlags DestinationAccess = 0;
    /// Dependency start in bytes; zero for a whole-buffer dependency.
    VkDeviceSize Offset = 0;
    /// Dependency length in bytes, or VK_WHOLE_SIZE.
    VkDeviceSize Size = VK_WHOLE_SIZE;
    /// Reason for the selected range.
    VulkanBufferRangeOutcome Outcome = VulkanBufferRangeOutcome::Control;
};
/// Declared byte range of one graph buffer use.
struct VulkanBufferRangeRequest final
{
    /// Nonzero graph execution identity; zero requests a whole-buffer dependency.
    foundation::uint64 Execution = 0;
    /// Declared use start in bytes.
    foundation::uint64 Offset = 0;
    /// Declared use length in bytes; zero requests a whole-buffer dependency.
    foundation::uint64 Size = 0;
};

// Thanks to Khronos, "Synchronization Examples", compute-to-compute and compute-to-graphics
// dependencies. This adapts the narrow stage/access pairs to Vulkan 1.1 barriers and retains
// the original writer for later independent reads.
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
/// Plan a dependency from inoutState's prior accesses to access and record the use in inoutState.
LUDUS_INLINE VulkanBufferBarrierPlan PlanVulkanBufferBarrier(const VulkanBufferAccess& access,
                                                             VulkanBufferState& inoutState) noexcept
{
    const VkAccessFlags destinationAccess = access.ReadAccess | access.WriteAccess;
    const VulkanBufferBarrierPlan plan
    {
        .SourceStages = inoutState.Stages | inoutState.WriterStages,
        .DestinationStages = access.Stages,
        .SourceAccess = inoutState.Access | inoutState.WriterAccess,
        .DestinationAccess = destinationAccess,
    };
    if (access.WriteAccess != 0)
    {
        inoutState.Stages = access.Stages;
        inoutState.WriterStages = access.Stages;
        inoutState.Access = destinationAccess;
        inoutState.WriterAccess = access.WriteAccess;
    }
    else
    {
        inoutState.Stages |= access.Stages;
        inoutState.Access |= destinationAccess;
    }
    return plan;
}
/// Add a use ordered only for part of the buffer to the conservative whole-buffer superset.
LUDUS_INLINE void AccumulateVulkanBufferAccess(const VulkanBufferAccess& access, VulkanBufferState& inoutState) noexcept
{
    inoutState.Stages |= access.Stages;
    inoutState.Access |= access.ReadAccess | access.WriteAccess;
    if (access.WriteAccess != 0)
    {
        inoutState.WriterStages |= access.Stages;
        inoutState.WriterAccess |= access.WriteAccess;
    }
}

/// Number of buffers whose ranges one graph execution may track.
inline constexpr foundation::usize VULKAN_RANGE_BUFFER_CAPACITY = 16;
/// Number of disjoint byte ranges one graph execution may track across all buffers.
inline constexpr foundation::usize VULKAN_BUFFER_RANGE_CAPACITY = 64;

/// Byte-range snapshots of buffers used by one graph execution.
///
/// The whole-buffer state stays authoritative outside the execution: every narrowed use is also
/// added to it as a superset, so transfers and later submissions keep ordering against all prior
/// accesses. A new execution identity discards every snapshot, which revalidates them after
/// direct updates, readbacks and other submissions. A use that declares no range, partly
/// overlaps a tracked range or exceeds capacity collapses the buffer to the whole-buffer plan.
/// Owner thread only.
class VulkanBufferRangeLedger final
{
public:
    /// Plan the barrier for a declared buffer use and record it in the ledger and inoutWhole.
    /// @param slot Backend buffer slot; ranges of different slots never interact.
    /// @param request Declared use range and its graph execution.
    /// @param access Stage and access scope of the use.
    /// @param inoutWhole Conservative whole-buffer state, updated for every outcome.
    /// @return Narrowed range plan, or the whole-buffer plan with its fallback reason.
    VulkanBufferBarrierPlan Plan(foundation::usize slot,
                                 const VulkanBufferRangeRequest& request,
                                 const VulkanBufferAccess& access,
                                 VulkanBufferState& inoutWhole) noexcept
    {
        if (request.Execution != mExecution)
        {
            Clear();
            mExecution = request.Execution;
        }
        if (request.Execution == 0 || request.Size == 0)
        {
            return collapse(slot, access, VulkanBufferRangeOutcome::WholeUse, inoutWhole);
        }
        const foundation::uint64 end = request.Offset + request.Size;
        foundation::usize match = INVALID;
        for (foundation::usize i = 0; i < mRangeCount; ++i)
        {
            const Range& range = mRanges[i];
            if (range.Slot != slot || range.Offset >= end || request.Offset >= range.Offset + range.Size)
            {
                continue;
            }
            if (range.Offset != request.Offset || range.Size != request.Size)
            {
                return collapse(slot, access, VulkanBufferRangeOutcome::PartialOverlap, inoutWhole);
            }
            match = i;
        }
        if (match == INVALID)
        {
            foundation::usize base = findBase(slot);
            if ((base == INVALID && mBaseCount == VULKAN_RANGE_BUFFER_CAPACITY) ||
                mRangeCount == VULKAN_BUFFER_RANGE_CAPACITY)
            {
                return collapse(slot, access, VulkanBufferRangeOutcome::Capacity, inoutWhole);
            }
            if (base == INVALID)
            {
                base = mBaseCount;
                mBases[base] = { .Slot = slot, .State = inoutWhole };
                ++mBaseCount;
            }
            match = mRangeCount;
            mRanges[match] =
            {
                .Slot = slot,
                .Offset = request.Offset,
                .Size = request.Size,
                .State = mBases[base].State,
            };
            ++mRangeCount;
        }
        VulkanBufferBarrierPlan plan = PlanVulkanBufferBarrier(access, mRanges[match].State);
        plan.Offset = request.Offset;
        plan.Size = request.Size;
        plan.Outcome = VulkanBufferRangeOutcome::Narrowed;
        AccumulateVulkanBufferAccess(access, inoutWhole);
        return plan;
    }
    /// Discard the snapshots of one buffer after a direct transfer or destruction.
    void Forget(foundation::usize slot) noexcept
    {
        foundation::usize kept = 0;
        for (foundation::usize i = 0; i < mRangeCount; ++i)
        {
            if (mRanges[i].Slot != slot)
            {
                mRanges[kept] = mRanges[i];
                ++kept;
            }
        }
        mRangeCount = kept;
        kept = 0;
        for (foundation::usize i = 0; i < mBaseCount; ++i)
        {
            if (mBases[i].Slot != slot)
            {
                mBases[kept] = mBases[i];
                ++kept;
            }
        }
        mBaseCount = kept;
    }
    /// Discard every snapshot.
    void Clear() noexcept
    {
        mRangeCount = 0;
        mBaseCount = 0;
        mExecution = 0;
    }
    /// Number of tracked byte ranges.
    [[nodiscard]] LUDUS_INLINE foundation::usize GetRangeCount() const noexcept
    {
        return mRangeCount;
    }

private:
    static constexpr foundation::usize INVALID = ~foundation::usize{0};
    struct Base final
    {
        foundation::usize Slot = 0;
        VulkanBufferState State;
    };
    struct Range final
    {
        foundation::usize Slot = 0;
        foundation::uint64 Offset = 0;
        foundation::uint64 Size = 0;
        VulkanBufferState State;
    };
    [[nodiscard]] foundation::usize findBase(foundation::usize slot) const noexcept
    {
        for (foundation::usize i = 0; i < mBaseCount; ++i)
        {
            if (mBases[i].Slot == slot)
            {
                return i;
            }
        }
        return INVALID;
    }
    VulkanBufferBarrierPlan collapse(foundation::usize slot,
                                     const VulkanBufferAccess& access,
                                     VulkanBufferRangeOutcome outcome,
                                     VulkanBufferState& inoutWhole) noexcept
    {
        Forget(slot);
        VulkanBufferBarrierPlan plan = PlanVulkanBufferBarrier(access, inoutWhole);
        plan.Outcome = outcome;
        return plan;
    }
    foundation::StaticArray<Base, VULKAN_RANGE_BUFFER_CAPACITY> mBases{};
    foundation::StaticArray<Range, VULKAN_BUFFER_RANGE_CAPACITY> mRanges{};
    foundation::usize mBaseCount = 0;
    foundation::usize mRangeCount = 0;
    foundation::uint64 mExecution = 0;
};
} // namespace ludus::graphics::rhi::backend
