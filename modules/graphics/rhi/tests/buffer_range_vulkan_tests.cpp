#include "internal/buffer_range_vulkan.h"
#include "internal/synchronization_trace_vulkan.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::uint64;
using ludus::foundation::usize;
using ludus::graphics::rhi::GraphAccessMode;
using ludus::graphics::rhi::backend::AccumulateVulkanBufferAccess;
using ludus::graphics::rhi::backend::GetVulkanBufferAccess;
using ludus::graphics::rhi::backend::PlanVulkanBufferBarrier;
using ludus::graphics::rhi::backend::VULKAN_BUFFER_RANGE_CAPACITY;
using ludus::graphics::rhi::backend::VULKAN_RANGE_BUFFER_CAPACITY;
using ludus::graphics::rhi::backend::VULKAN_SYNCHRONIZATION_RECORD_CAPACITY;
using ludus::graphics::rhi::backend::VulkanBufferAccess;
using ludus::graphics::rhi::backend::VulkanBufferBarrierPlan;
using ludus::graphics::rhi::backend::VulkanBufferRangeLedger;
using ludus::graphics::rhi::backend::VulkanBufferRangeOutcome;
using ludus::graphics::rhi::backend::VulkanBufferState;
using ludus::graphics::rhi::backend::VulkanSynchronizationRecord;
using ludus::graphics::rhi::backend::VulkanSynchronizationTrace;

namespace
{
constexpr usize BUFFER = 3;
constexpr usize OTHER_BUFFER = 4;
constexpr uint64 FIRST_EXECUTION = 1;
constexpr uint64 SECOND_EXECUTION = 2;
constexpr uint64 RANGE_BYTES = 4;
constexpr uint64 SECOND_RANGE_OFFSET = 256;
constexpr VulkanBufferState TRANSFER_WRITTEN
{
    .Stages = VK_PIPELINE_STAGE_TRANSFER_BIT,
    .Access = VK_ACCESS_TRANSFER_WRITE_BIT,
    .WriterStages = VK_PIPELINE_STAGE_TRANSFER_BIT,
    .WriterAccess = VK_ACCESS_TRANSFER_WRITE_BIT,
};
constexpr VulkanBufferAccess STORAGE_WRITE = GetVulkanBufferAccess(GraphAccessMode::StorageReadWrite, true);
constexpr VulkanBufferAccess STORAGE_READ = GetVulkanBufferAccess(GraphAccessMode::StorageRead, true);
constexpr VulkanBufferAccess VERTEX_READ = GetVulkanBufferAccess(GraphAccessMode::Vertex, true);
} // namespace

TEST_CASE("Vulkan whole-buffer control keeps the retained writer across readers", "[rhi][vulkan][synchronization]")
{
    VulkanBufferState state = TRANSFER_WRITTEN;
    const VulkanBufferBarrierPlan write = PlanVulkanBufferBarrier(STORAGE_WRITE, state);
    CHECK(write.SourceStages == VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK(write.SourceAccess == VK_ACCESS_TRANSFER_WRITE_BIT);
    CHECK(write.DestinationStages == STORAGE_WRITE.Stages);
    CHECK(write.DestinationAccess == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
    CHECK(write.Offset == 0);
    CHECK(write.Size == VK_WHOLE_SIZE);
    CHECK(write.Outcome == VulkanBufferRangeOutcome::Control);
    CHECK(state.WriterStages == STORAGE_WRITE.Stages);
    CHECK(state.WriterAccess == VK_ACCESS_SHADER_WRITE_BIT);

    const VulkanBufferBarrierPlan vertex = PlanVulkanBufferBarrier(VERTEX_READ, state);
    CHECK(vertex.SourceStages == STORAGE_WRITE.Stages);
    CHECK(vertex.DestinationStages == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    // A later reader still waits for the writer and also for the vertex reader before it.
    const VulkanBufferBarrierPlan read = PlanVulkanBufferBarrier(STORAGE_READ, state);
    CHECK(read.SourceStages == (STORAGE_WRITE.Stages | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT));
    CHECK((read.SourceAccess & VK_ACCESS_SHADER_WRITE_BIT) == VK_ACCESS_SHADER_WRITE_BIT);
    CHECK((read.SourceAccess & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) == VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
}

TEST_CASE("Vulkan buffer range trial narrows disjoint and dependent ranges", "[rhi][vulkan][synchronization]")
{
    VulkanBufferRangeLedger ledger;
    VulkanBufferState whole = TRANSFER_WRITTEN;
    const VulkanBufferBarrierPlan first =
        ledger.Plan(BUFFER, { .Execution = FIRST_EXECUTION, .Offset = 0, .Size = RANGE_BYTES }, STORAGE_WRITE, whole);
    CHECK(first.Outcome == VulkanBufferRangeOutcome::Narrowed);
    CHECK(first.Offset == 0);
    CHECK(first.Size == RANGE_BYTES);
    CHECK(first.SourceStages == VK_PIPELINE_STAGE_TRANSFER_BIT);

    SECTION("A disjoint write waits only for the accesses before the execution")
    {
        const VulkanBufferBarrierPlan second =
            ledger.Plan(BUFFER,
                        { .Execution = FIRST_EXECUTION, .Offset = SECOND_RANGE_OFFSET, .Size = RANGE_BYTES },
                        STORAGE_WRITE,
                        whole);
        CHECK(second.Outcome == VulkanBufferRangeOutcome::Narrowed);
        CHECK(second.Offset == SECOND_RANGE_OFFSET);
        CHECK(second.Size == RANGE_BYTES);
        CHECK(second.SourceStages == VK_PIPELINE_STAGE_TRANSFER_BIT);
        CHECK(second.SourceAccess == VK_ACCESS_TRANSFER_WRITE_BIT);
        CHECK(ledger.GetRangeCount() == 2);
    }
    SECTION("A dependent write of the same range waits for the earlier writer")
    {
        const VulkanBufferBarrierPlan second = ledger.Plan(BUFFER,
                                                           {
                                                               .Execution = FIRST_EXECUTION,
                                                               .Offset = 0,
                                                               .Size = RANGE_BYTES,
                                                           },
                                                           STORAGE_WRITE,
                                                           whole);
        CHECK(second.Outcome == VulkanBufferRangeOutcome::Narrowed);
        CHECK(second.SourceStages == STORAGE_WRITE.Stages);
        CHECK(second.SourceAccess == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
        CHECK(ledger.GetRangeCount() == 1);
    }
    SECTION("Another buffer keeps independent snapshots")
    {
        VulkanBufferState other = TRANSFER_WRITTEN;
        const VulkanBufferBarrierPlan second =
            ledger.Plan(OTHER_BUFFER,
                        { .Execution = FIRST_EXECUTION, .Offset = 0, .Size = RANGE_BYTES },
                        STORAGE_READ,
                        other);
        CHECK(second.Outcome == VulkanBufferRangeOutcome::Narrowed);
        CHECK(second.SourceStages == VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    // Every narrowed use also enters the conservative whole-buffer superset.
    CHECK((whole.Stages & VK_PIPELINE_STAGE_TRANSFER_BIT) == VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK((whole.WriterStages & STORAGE_WRITE.Stages) == STORAGE_WRITE.Stages);
    CHECK((whole.WriterAccess & VK_ACCESS_SHADER_WRITE_BIT) == VK_ACCESS_SHADER_WRITE_BIT);
}

TEST_CASE("Vulkan buffer range trial falls back to whole-buffer ordering", "[rhi][vulkan][synchronization]")
{
    VulkanBufferRangeLedger ledger;
    VulkanBufferState whole = TRANSFER_WRITTEN;
    static_cast<void>(
        ledger.Plan(BUFFER, { .Execution = FIRST_EXECUTION, .Offset = 0, .Size = RANGE_BYTES }, STORAGE_WRITE, whole));
    static_cast<void>(ledger.Plan(BUFFER,
                                  { .Execution = FIRST_EXECUTION, .Offset = SECOND_RANGE_OFFSET, .Size = RANGE_BYTES },
                                  VERTEX_READ,
                                  whole));

    SECTION("A partly overlapping use orders every prior access of the buffer")
    {
        const VulkanBufferBarrierPlan overlap =
            ledger.Plan(BUFFER,
                        { .Execution = FIRST_EXECUTION, .Offset = 0, .Size = SECOND_RANGE_OFFSET + RANGE_BYTES },
                        STORAGE_READ,
                        whole);
        CHECK(overlap.Outcome == VulkanBufferRangeOutcome::PartialOverlap);
        CHECK(overlap.Offset == 0);
        CHECK(overlap.Size == VK_WHOLE_SIZE);
        CHECK(overlap.SourceStages ==
              (VK_PIPELINE_STAGE_TRANSFER_BIT | STORAGE_WRITE.Stages | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT));
        CHECK(ledger.GetRangeCount() == 0);
        // The collapsed buffer opens a new range from the post-barrier whole state.
        const VulkanBufferBarrierPlan after = ledger.Plan(BUFFER,
                                                          {
                                                              .Execution = FIRST_EXECUTION,
                                                              .Offset = 0,
                                                              .Size = RANGE_BYTES,
                                                          },
                                                          STORAGE_WRITE,
                                                          whole);
        CHECK(after.Outcome == VulkanBufferRangeOutcome::Narrowed);
        CHECK((after.SourceStages & STORAGE_READ.Stages) == STORAGE_READ.Stages);
    }
    SECTION("A use without a declared range collapses the buffer")
    {
        const VulkanBufferBarrierPlan restoration =
            ledger.Plan(BUFFER, { .Execution = FIRST_EXECUTION, .Offset = 0, .Size = 0 }, VERTEX_READ, whole);
        CHECK(restoration.Outcome == VulkanBufferRangeOutcome::WholeUse);
        CHECK(restoration.Size == VK_WHOLE_SIZE);
        CHECK((restoration.SourceStages & STORAGE_WRITE.Stages) == STORAGE_WRITE.Stages);
        CHECK(ledger.GetRangeCount() == 0);
    }
    SECTION("A new execution revalidates snapshots from the whole-buffer state")
    {
        const VulkanBufferBarrierPlan next =
            ledger.Plan(BUFFER,
                        { .Execution = SECOND_EXECUTION, .Offset = 0, .Size = RANGE_BYTES },
                        STORAGE_WRITE,
                        whole);
        CHECK(next.Outcome == VulkanBufferRangeOutcome::Narrowed);
        CHECK(next.SourceStages ==
              (VK_PIPELINE_STAGE_TRANSFER_BIT | STORAGE_WRITE.Stages | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT));
        CHECK(ledger.GetRangeCount() == 1);
    }
    SECTION("A direct transfer forgets the buffer's snapshots")
    {
        ledger.Forget(BUFFER);
        CHECK(ledger.GetRangeCount() == 0);
        whole = TRANSFER_WRITTEN;
        const VulkanBufferBarrierPlan next = ledger.Plan(BUFFER,
                                                         {
                                                             .Execution = FIRST_EXECUTION,
                                                             .Offset = 0,
                                                             .Size = RANGE_BYTES,
                                                         },
                                                         STORAGE_READ,
                                                         whole);
        CHECK(next.SourceStages == VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
}

TEST_CASE("Vulkan buffer range trial reports its capacity limits", "[rhi][vulkan][synchronization]")
{
    VulkanBufferRangeLedger ledger;
    VulkanBufferState whole = TRANSFER_WRITTEN;
    SECTION("Ranges")
    {
        for (usize i = 0; i < VULKAN_BUFFER_RANGE_CAPACITY; ++i)
        {
            const VulkanBufferBarrierPlan plan =
                ledger.Plan(BUFFER,
                            { .Execution = FIRST_EXECUTION, .Offset = i * SECOND_RANGE_OFFSET, .Size = RANGE_BYTES },
                            STORAGE_WRITE,
                            whole);
            REQUIRE(plan.Outcome == VulkanBufferRangeOutcome::Narrowed);
        }
        const VulkanBufferBarrierPlan full =
            ledger.Plan(BUFFER,
                        {
                            .Execution = FIRST_EXECUTION,
                            .Offset = VULKAN_BUFFER_RANGE_CAPACITY * SECOND_RANGE_OFFSET,
                            .Size = RANGE_BYTES,
                        },
                        STORAGE_WRITE,
                        whole);
        CHECK(full.Outcome == VulkanBufferRangeOutcome::Capacity);
        CHECK(full.Size == VK_WHOLE_SIZE);
        CHECK(ledger.GetRangeCount() == 0);
    }
    SECTION("Buffers")
    {
        for (usize i = 0; i < VULKAN_RANGE_BUFFER_CAPACITY; ++i)
        {
            const VulkanBufferBarrierPlan plan =
                ledger.Plan(i, { .Execution = FIRST_EXECUTION, .Offset = 0, .Size = RANGE_BYTES }, STORAGE_READ, whole);
            REQUIRE(plan.Outcome == VulkanBufferRangeOutcome::Narrowed);
        }
        const VulkanBufferBarrierPlan full =
            ledger.Plan(VULKAN_RANGE_BUFFER_CAPACITY,
                        { .Execution = FIRST_EXECUTION, .Offset = 0, .Size = RANGE_BYTES },
                        STORAGE_READ,
                        whole);
        CHECK(full.Outcome == VulkanBufferRangeOutcome::Capacity);
        CHECK(ledger.GetRangeCount() == VULKAN_RANGE_BUFFER_CAPACITY);
    }
}

TEST_CASE("Vulkan accumulated range accesses form a whole-buffer superset", "[rhi][vulkan][synchronization]")
{
    VulkanBufferState state = TRANSFER_WRITTEN;
    AccumulateVulkanBufferAccess(VERTEX_READ, state);
    CHECK(state.WriterStages == VK_PIPELINE_STAGE_TRANSFER_BIT);
    AccumulateVulkanBufferAccess(STORAGE_WRITE, state);
    CHECK(state.Stages == (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | STORAGE_WRITE.Stages));
    CHECK(state.WriterStages == (VK_PIPELINE_STAGE_TRANSFER_BIT | STORAGE_WRITE.Stages));
    CHECK(state.WriterAccess == (VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT));
}

TEST_CASE("Vulkan synchronization trace is opt-in and bounded", "[rhi][vulkan][synchronization]")
{
    VulkanSynchronizationTrace trace;
    const VulkanSynchronizationRecord record{ .Scope = 1, .Size = VK_WHOLE_SIZE };
    trace.Append(record);
    CHECK(trace.GetCount() == 0);
    CHECK(trace.GetDropped() == 0);

    trace.SetEnabled(true);
    for (usize i = 0; i < VULKAN_SYNCHRONIZATION_RECORD_CAPACITY; ++i)
    {
        trace.Append(record);
    }
    trace.Append(record);
    trace.Append(record);
    CHECK(trace.GetCount() == VULKAN_SYNCHRONIZATION_RECORD_CAPACITY);
    CHECK(trace.GetDropped() == 2);
    CHECK(trace.GetRecord(0).Scope == 1);
    CHECK(trace.GetRecord(0).Size == VK_WHOLE_SIZE);

    trace.Clear();
    CHECK(trace.IsEnabled() == true);
    CHECK(trace.GetCount() == 0);
    CHECK(trace.GetDropped() == 0);
    trace.SetEnabled(false);
    trace.Append(record);
    CHECK(trace.GetCount() == 0);
}
