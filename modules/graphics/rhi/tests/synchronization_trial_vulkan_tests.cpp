#include "internal/synchronization_trace_vulkan.h"
#include "ordered_advance.h"
#include "ordered_draw.h"
#include "ordered_prepare.h"
#include "raster_test_readiness.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <cstring>

#include <catch2/catch_test_macros.hpp>

// R13a evidence: the ordered-compute fixture's dispatch -> indirect raster -> transfer readback
// chain, extended with disjoint, dependent and partly overlapping ranges of one storage buffer.
// The whole-buffer control and the opt-in range trial must produce identical bytes and pixels;
// the backend-private trace must show the narrower native dependency only where intended.
// Run with VK_LAYER_KHRONOS_validation and VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true in CI.
using namespace ludus::foundation;
using ludus::graphics::rhi::testing::RequireReady;
namespace rhi = ludus::graphics::rhi;
namespace backend = ludus::graphics::rhi::backend;
namespace
{
constexpr uint32 WIDTH = 64;
constexpr uint32 HEIGHT = 48;
constexpr usize CHANNELS = 4;
constexpr usize PIXEL_BYTES = usize{WIDTH} * HEIGHT * CHANNELS;
constexpr usize COUNTER_COUNT = 3;
constexpr usize PREPARE_BINDING_COUNT = 3;
constexpr usize DRAW_USE_COUNT = 4;
constexpr uint32 TRIANGLE_INDEX_COUNT = 3;
constexpr usize FLOAT2_VALUES = 2;
constexpr usize TRIANGLE_COORDINATES = TRIANGLE_INDEX_COUNT * FLOAT2_VALUES;
constexpr usize COORDINATE_STRIDE = sizeof(float32) * FLOAT2_VALUES;
// The prepare shader draws one instance only when counter 0 reaches 42.
constexpr StaticArray<uint32, COUNTER_COUNT> COUNTER_SEEDS{40, 7, 100};
constexpr StaticArray<uint32, COUNTER_COUNT> COUNTERS_AFTER_FIRST_GRAPH{42, 8, 101};
// GPU scope identities of the first graph's passes (graph pass index plus one).
constexpr uint64 FIRST_ADVANCE_SCOPE = 1;
constexpr uint64 DISJOINT_ADVANCE_SCOPE = 2;
constexpr uint64 DEPENDENT_ADVANCE_SCOPE = 3;
constexpr uint64 OVERLAPPING_ADVANCE_SCOPE = 4;
constexpr uint64 PREPARE_SCOPE = 5;
constexpr usize FIRST_GRAPH_COMPUTE_PASSES = 5;

struct DeviceOwner final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    DeviceOwner()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Required.Compute = true;
        description.EnableGpuTiming = true;
        REQUIRE(rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface) ==
                rhi::DeviceStatus::Ready);
    }
    DeviceOwner(const DeviceOwner&) = delete;
    DeviceOwner& operator=(const DeviceOwner&) = delete;
    ~DeviceOwner()
    {
        static_cast<void>(rhi::DestroyDevice(Device));
    }
};
// Restore the default control and disable recording even when an assertion fails.
struct TrialSelection final
{
    explicit TrialSelection(bool isTrial) noexcept
    {
        backend::SetVulkanBufferRangeTrial(isTrial);
        backend::SetVulkanSynchronizationTrace(true);
    }
    TrialSelection(const TrialSelection&) = delete;
    TrialSelection& operator=(const TrialSelection&) = delete;
    ~TrialSelection()
    {
        backend::SetVulkanBufferRangeTrial(false);
        backend::SetVulkanSynchronizationTrace(false);
    }
};
struct ScenarioResult final
{
    Array<uint8> CountersAfterFirstGraph;
    uint32 CounterAfterSecondGraph = 0;
    StaticArray<uint8, PIXEL_BYTES> Pixels{};
    time::Duration EncodeCpu;
    rhi::GpuPassTimings Timings;
};
const backend::VulkanSynchronizationRecord* FindBufferRecord(const backend::VulkanSynchronizationTrace& trace,
                                                             backend::VulkanSynchronizationSite site,
                                                             uint64 scope,
                                                             uint64 execution)
{
    for (usize i = 0; i < trace.GetCount(); ++i)
    {
        const backend::VulkanSynchronizationRecord& record = trace.GetRecord(i);
        if (record.Site == site && record.Object == backend::VulkanSynchronizationObject::Buffer &&
            record.Scope == scope && (execution == 0 || record.Execution == execution))
        {
            return &record;
        }
    }
    return nullptr;
}
float64 SumAvailableGpuNanoseconds(const rhi::GpuPassTimings& timings)
{
    float64 sum = 0;
    for (usize i = 0; i < timings.Count; ++i)
    {
        if (timings.Passes[i].Timing.State == rhi::GpuTimingState::Available)
        {
            sum += timings.Passes[i].Timing.DurationNanoseconds;
        }
    }
    return sum;
}
rhi::ComputeDispatch MakeDispatch(rhi::DeviceHandle device,
                                  const rhi::RasterShaderDescription& description,
                                  BorrowedView<const rhi::RasterBindingResource> resources)
{
    rhi::RasterShaderHandle shader;
    rhi::BindingLayoutHandle layout;
    rhi::ComputeDispatch dispatch;
    REQUIRE(rhi::CreateRasterShader(device, description, shader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingLayout(device, description.Bindings, layout) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingSet(device, layout, resources, dispatch.Bindings) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateComputePipeline(device, {shader, layout}, dispatch.Pipeline) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, shader) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, layout) == rhi::RasterStatus::Ready);
    return dispatch;
}
void UpdateBuffer(rhi::DeviceHandle device, rhi::BufferHandle buffer, usize offset, BorrowedView<const uint8> bytes)
{
    rhi::TransferTicket update;
    REQUIRE(rhi::RequestBufferUpdate(device, buffer, offset, bytes.GetDataOrNull(), bytes.GetSize(), update) ==
            rhi::RasterStatus::Pending);
    RequireReady(device, update);
    REQUIRE(rhi::Release(device, update) == rhi::RasterStatus::Ready);
}
void ReadBuffer(rhi::DeviceHandle device, rhi::BufferHandle buffer, usize offset, usize size, uint8* outBytes)
{
    rhi::ReadbackTicket readback;
    REQUIRE(rhi::RequestBufferReadback(device, buffer, offset, size, readback) == rhi::RasterStatus::Pending);
    RequireReady(device, readback);
    REQUIRE(rhi::CopyReadback(device, readback, outBytes, size) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(device, readback) == rhi::RasterStatus::Ready);
}
void AddAdvancePass(rhi::DeviceHandle device,
                    rhi::OrderedGraph graph,
                    const char* name,
                    rhi::GraphVersion& inoutCounters,
                    const rhi::ComputeDispatch& dispatch,
                    usize useOffset,
                    usize useSize)
{
    rhi::GraphVersion next;
    REQUIRE(rhi::NextGraphVersion(device, graph, inoutCounters, next) == rhi::RasterStatus::Ready);
    const rhi::GraphUse use{next,
                            rhi::GraphAccessMode::StorageReadWrite,
                            useOffset,
                            useSize,
                            rhi::RasterVisibility::Compute};
    REQUIRE(rhi::AddGraphPass(device,
                              graph,
                              {
                                  .Name = name,
                                  .Attachment = {},
                                  .Uses = &use,
                                  .UseCount = 1,
                                  .Dispatch = &dispatch,
                              }) == rhi::RasterStatus::Ready);
    inoutCounters = next;
}

// One storage buffer holds three counters, each at a storage-offset-aligned stride:
// pass 1 advances counter 0, pass 2 counter 1 (disjoint), pass 3 counter 0 again (dependent write
// in the same storage role), pass 4 counter 2 through a use that also covers counter 1 (partial
// overlap), and pass 5 reads counter 0 to generate an indirect draw. A readback in its own
// submission, a direct update and a second graph then cross submission boundaries.
void RunScenario(const DeviceOwner& owner, bool isTrial, ScenarioResult& outResult)
{
    const rhi::DeviceHandle device = owner.Device;
    const TrialSelection selection(isTrial);

    rhi::ComputeCapabilities capabilities;
    REQUIRE(rhi::GetComputeCapabilities(device, capabilities) == rhi::RasterStatus::Ready);
    const usize stride =
        capabilities.StorageOffsetAlignment > sizeof(uint32) ? capabilities.StorageOffsetAlignment : sizeof(uint32);
    const usize counterBytes = stride * COUNTER_COUNT;
    Array<uint8> counterValues;
    REQUIRE(counterValues.TryResize(counterBytes) == true);
    std::memset(counterValues.GetData(), 0, counterBytes);
    rhi::BufferHandle counters;
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::Storage, counterBytes},
                              {counterValues.GetData(), counterBytes},
                              counters) == rhi::RasterStatus::Ready);
    for (usize i = 0; i < COUNTER_COUNT; ++i)
    {
        std::memcpy(counterValues.GetData() + i * stride, &COUNTER_SEEDS[i], sizeof(uint32));
    }
    UpdateBuffer(device, counters, 0, {counterValues.GetData(), counterBytes});

    const StaticArray<float32, TRIANGLE_COORDINATES> initialVertices{};
    const rhi::IndexedIndirectArguments initialArguments{};
    const StaticArray<uint32, TRIANGLE_INDEX_COUNT> initialIndices{0, 1, 2};
    rhi::BufferHandle vertices;
    rhi::BufferHandle arguments;
    rhi::BufferHandle indices;
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::StorageVertex, sizeof(initialVertices)},
                              {reinterpret_cast<const uint8*>(initialVertices.GetData()), sizeof(initialVertices)},
                              vertices) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::StorageIndirect, sizeof(initialArguments)},
                              {reinterpret_cast<const uint8*>(&initialArguments), sizeof(initialArguments)},
                              arguments) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(device,
                              {rhi::BufferRole::Index32, sizeof(initialIndices)},
                              {reinterpret_cast<const uint8*>(initialIndices.GetData()), sizeof(initialIndices)},
                              indices) == rhi::RasterStatus::Ready);

    const rhi::RasterShaderDescription advanceShader = ludus::shaders::ordered_advance::Compute();
    const rhi::RasterShaderDescription prepareShader = ludus::shaders::ordered_prepare::Compute();
    StaticArray<rhi::ComputeDispatch, COUNTER_COUNT> advances{};
    for (usize i = 0; i < COUNTER_COUNT; ++i)
    {
        const rhi::RasterBindingResource binding{advanceShader.Bindings[0].Binding,
                                                 counters,
                                                 i * stride,
                                                 sizeof(uint32)};
        advances[i] = MakeDispatch(device, advanceShader, {&binding, 1});
    }
    const StaticArray<rhi::RasterBindingResource, PREPARE_BINDING_COUNT> prepareBindings{{
        {prepareShader.Bindings[0].Binding, counters, 0, sizeof(uint32)},
        {prepareShader.Bindings[1].Binding, vertices, 0, sizeof(initialVertices)},
        {prepareShader.Bindings[2].Binding, arguments, 0, sizeof(initialArguments)},
    }};
    rhi::ComputeDispatch prepare =
        MakeDispatch(device, prepareShader, {prepareBindings.GetData(), prepareBindings.GetSize()});

    rhi::RasterShaderHandle vertex;
    rhi::RasterShaderHandle fragment;
    rhi::BindingLayoutHandle rasterLayout;
    rhi::BindingSetHandle rasterSet;
    rhi::RasterPipelineHandle rasterPipeline;
    REQUIRE(rhi::CreateRasterShader(device, ludus::shaders::ordered_draw::Vertex(), vertex) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(device, ludus::shaders::ordered_draw::Fragment(), fragment) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingLayout(device, {}, rasterLayout) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBindingSet(device, rasterLayout, {}, rasterSet) == rhi::RasterStatus::Ready);
    const rhi::RasterVertexStream stream{COORDINATE_STRIDE, false};
    const rhi::RasterVertexAttribute attribute{0, 0, 0, rhi::RasterVertexFormat::Float2};
    REQUIRE(rhi::CreateRasterPipeline(device,
                                      {vertex,
                                       fragment,
                                       rasterLayout,
                                       {&stream, 1},
                                       {&attribute, 1},
                                       false,
                                       false,
                                       rhi::RasterTarget::Rgba8Unorm},
                                      rasterPipeline) == rhi::RasterStatus::Ready);
    rhi::TextureHandle target;
    REQUIRE(rhi::CreateTexture(device, { .Width = WIDTH, .Height = HEIGHT, .Attachment = true }, {}, target) ==
            rhi::RasterStatus::Ready);
    rhi::RasterTextureState targetState;
    REQUIRE(rhi::GetTextureState(device, target, targetState) == rhi::RasterStatus::Ready);

    rhi::OrderedGraph graph;
    rhi::GraphVersion counterVersion;
    rhi::GraphVersion vertexInput;
    rhi::GraphVersion indirectInput;
    rhi::GraphVersion indexInput;
    rhi::GraphVersion targetInput;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, counters, counterVersion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, vertices, vertexInput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, arguments, indirectInput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, indices, indexInput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphTexture(device,
                                    graph,
                                    target,
                                    targetState,
                                    rhi::RasterTextureUse::ColorAttachment,
                                    targetInput) == rhi::RasterStatus::Ready);
    AddAdvancePass(device, graph, "advance counter 0", counterVersion, advances[0], 0, sizeof(uint32));
    AddAdvancePass(device, graph, "advance counter 1", counterVersion, advances[1], stride, sizeof(uint32));
    AddAdvancePass(device, graph, "advance counter 0 again", counterVersion, advances[0], 0, sizeof(uint32));
    AddAdvancePass(device,
                   graph,
                   "advance counter 2 over counter 1",
                   counterVersion,
                   advances[2],
                   stride,
                   stride + sizeof(uint32));

    rhi::GraphVersion vertexOutput;
    rhi::GraphVersion indirectOutput;
    REQUIRE(rhi::NextGraphVersion(device, graph, vertexInput, vertexOutput) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, indirectInput, indirectOutput) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, PREPARE_BINDING_COUNT> prepareUses{{
        {counterVersion, rhi::GraphAccessMode::StorageRead, 0, sizeof(uint32), rhi::RasterVisibility::Compute},
        {vertexOutput,
         rhi::GraphAccessMode::StorageReadWrite,
         0,
         sizeof(initialVertices),
         rhi::RasterVisibility::Compute},
        {indirectOutput,
         rhi::GraphAccessMode::StorageReadWrite,
         0,
         sizeof(initialArguments),
         rhi::RasterVisibility::Compute},
    }};
    REQUIRE(rhi::AddGraphPass(device,
                              graph,
                              {
                                  .Name = "prepare indirect draw",
                                  .Attachment = {},
                                  .Uses = prepareUses.GetData(),
                                  .UseCount = prepareUses.GetSize(),
                                  .Dispatch = &prepare,
                              }) == rhi::RasterStatus::Ready);
    rhi::GraphVersion targetOutput;
    REQUIRE(rhi::NextGraphVersion(device, graph, targetInput, targetOutput) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, DRAW_USE_COUNT> drawUses{{
        {vertexOutput, rhi::GraphAccessMode::Vertex, 0, sizeof(initialVertices)},
        {indirectOutput, rhi::GraphAccessMode::Indirect, 0, sizeof(initialArguments)},
        {indexInput, rhi::GraphAccessMode::Index, 0, sizeof(initialIndices)},
        {targetOutput, rhi::GraphAccessMode::ColorWrite},
    }};
    const rhi::RasterVertexSlice slice{vertices, 0, sizeof(initialVertices)};
    const rhi::RasterDraw draw
    {
        .Pipeline = rasterPipeline,
        .Bindings = rasterSet,
        .Vertices = {&slice, 1},
        .Indices = indices,
        .IndexCount = TRIANGLE_INDEX_COUNT,
        .VertexCount = TRIANGLE_INDEX_COUNT,
        .InstanceCount = 1,
        .Indirect = arguments,
    };
    REQUIRE(rhi::AddGraphPass(device,
                              graph,
                              {
                                  .Name = "draw generated triangle",
                                  .Attachment = { .Color = target },
                                  .Uses = drawUses.GetData(),
                                  .UseCount = drawUses.GetSize(),
                                  .Draws = &draw,
                                  .DrawCount = 1,
                              }) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::AddGraphRoot(device, graph, targetOutput, rhi::GraphRoot::Readback) == rhi::RasterStatus::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == rhi::RasterStatus::Ready);
    REQUIRE(report.ComputePasses == FIRST_GRAPH_COMPUTE_PASSES);
    REQUIRE(report.RasterPasses == 1);

    const rhi::RasterStatus armed = rhi::ArmGpuPassTimings(device);
    REQUIRE((armed == rhi::RasterStatus::Ready || armed == rhi::RasterStatus::Unsupported));
    rhi::SubmissionToken completion;
    const time::Timestamp encodeStart = time::Now();
    REQUIRE(rhi::ExecuteOrderedGraph(device, owner.Surface, graph, completion) == rhi::RasterStatus::Ready);
    REQUIRE(time::TryElapsed(encodeStart, time::Now(), outResult.EncodeCpu) == time::TimeStatus::Ok);

    REQUIRE(outResult.CountersAfterFirstGraph.TryResize(counterBytes) == true);
    ReadBuffer(device, counters, 0, counterBytes, outResult.CountersAfterFirstGraph.GetData());
    constexpr usize ROW_BYTES = WIDTH * CHANNELS;
    rhi::ReadbackTicket imageReadback;
    REQUIRE(rhi::RequestTextureReadback(device, target, {0, 0, 0, WIDTH, HEIGHT}, ROW_BYTES, imageReadback) ==
            rhi::RasterStatus::Pending);
    RequireReady(device, imageReadback);
    REQUIRE(rhi::CopyReadback(device, imageReadback, outResult.Pixels.GetData(), outResult.Pixels.GetSize()) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(device, imageReadback) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetStatus(device, completion) == rhi::RasterStatus::Ready);
    if (armed == rhi::RasterStatus::Ready)
    {
        REQUIRE(rhi::GetGpuPassTimings(device, outResult.Timings) == rhi::RasterStatus::Ready);
    }

    // A direct update between submissions makes any first-graph snapshot stale.
    const uint32 zero = 0;
    UpdateBuffer(device, counters, stride, {reinterpret_cast<const uint8*>(&zero), sizeof(zero)});
    rhi::OrderedGraph second;
    REQUIRE(rhi::CreateOrderedGraph(device, second) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, second, counters, counterVersion) == rhi::RasterStatus::Ready);
    AddAdvancePass(device, second, "advance updated counter 1", counterVersion, advances[1], stride, sizeof(uint32));
    REQUIRE(rhi::AddGraphRoot(device, second, counterVersion, rhi::GraphRoot::Readback) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CompileOrderedGraph(device, second, report) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken secondCompletion;
    REQUIRE(rhi::ExecuteOrderedGraph(device, owner.Surface, second, secondCompletion) == rhi::RasterStatus::Ready);
    ReadBuffer(device, counters, stride, sizeof(uint32), reinterpret_cast<uint8*>(&outResult.CounterAfterSecondGraph));

    const backend::VulkanSynchronizationTrace& trace = backend::GetVulkanSynchronizationTrace();
    REQUIRE(trace.GetDropped() == 0);
    using Site = backend::VulkanSynchronizationSite;
    using Outcome = backend::VulkanBufferRangeOutcome;
    const backend::VulkanSynchronizationRecord* firstOrNull =
        FindBufferRecord(trace, Site::GraphPass, FIRST_ADVANCE_SCOPE, 0);
    REQUIRE(firstOrNull != nullptr);
    const uint64 firstExecution = firstOrNull->Execution;
    const uint64 firstSubmission = firstOrNull->Submission;
    CHECK(firstExecution != 0);
    CHECK(firstSubmission != 0);
    const backend::VulkanSynchronizationRecord* disjointOrNull =
        FindBufferRecord(trace, Site::GraphPass, DISJOINT_ADVANCE_SCOPE, firstExecution);
    const backend::VulkanSynchronizationRecord* dependentOrNull =
        FindBufferRecord(trace, Site::GraphPass, DEPENDENT_ADVANCE_SCOPE, firstExecution);
    const backend::VulkanSynchronizationRecord* overlapOrNull =
        FindBufferRecord(trace, Site::GraphPass, OVERLAPPING_ADVANCE_SCOPE, firstExecution);
    const backend::VulkanSynchronizationRecord* prepareOrNull =
        FindBufferRecord(trace, Site::GraphPass, PREPARE_SCOPE, firstExecution);
    REQUIRE(disjointOrNull != nullptr);
    REQUIRE(dependentOrNull != nullptr);
    REQUIRE(overlapOrNull != nullptr);
    REQUIRE(prepareOrNull != nullptr);
    CHECK(disjointOrNull->Submission == firstSubmission);
    CHECK(disjointOrNull->GraphResource == firstOrNull->GraphResource);
    CHECK(disjointOrNull->Slot == firstOrNull->Slot);
    const usize counterSlot = firstOrNull->Slot;
    // The seed update's transfer write precedes the first graph in every mode.
    CHECK((firstOrNull->SourceStages & VK_PIPELINE_STAGE_TRANSFER_BIT) == VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK((dependentOrNull->SourceStages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) ==
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    CHECK((dependentOrNull->SourceAccess & VK_ACCESS_SHADER_WRITE_BIT) == VK_ACCESS_SHADER_WRITE_BIT);
    if (isTrial == true)
    {
        CHECK(firstOrNull->Range == Outcome::Narrowed);
        CHECK(firstOrNull->Offset == 0);
        CHECK(firstOrNull->Size == sizeof(uint32));
        // Intended narrower operation: the disjoint counter does not wait for counter 0's dispatch.
        CHECK(disjointOrNull->Range == Outcome::Narrowed);
        CHECK(disjointOrNull->Offset == stride);
        CHECK(disjointOrNull->Size == sizeof(uint32));
        CHECK((disjointOrNull->SourceStages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) == 0);
        CHECK((disjointOrNull->SourceAccess & VK_ACCESS_SHADER_WRITE_BIT) == 0);
        CHECK(dependentOrNull->Range == Outcome::Narrowed);
        CHECK(dependentOrNull->Offset == 0);
        CHECK(dependentOrNull->Size == sizeof(uint32));
        CHECK(overlapOrNull->Range == Outcome::PartialOverlap);
        CHECK(overlapOrNull->Size == VK_WHOLE_SIZE);
        CHECK((overlapOrNull->SourceStages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) ==
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        CHECK(prepareOrNull->Range == Outcome::Narrowed);
        CHECK((prepareOrNull->SourceStages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) ==
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    else
    {
        const StaticArray<const backend::VulkanSynchronizationRecord*, FIRST_GRAPH_COMPUTE_PASSES> records{
            {firstOrNull, disjointOrNull, dependentOrNull, overlapOrNull, prepareOrNull}};
        for (const backend::VulkanSynchronizationRecord* record : records)
        {
            CHECK(record->Range == Outcome::Control);
            CHECK(record->Size == VK_WHOLE_SIZE);
        }
        CHECK((disjointOrNull->SourceStages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) ==
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    bool hasFinalCounter = false;
    bool hasCounterReadback = false;
    bool hasStagingReadback = false;
    bool hasGraphImage = false;
    bool hasImageReadback = false;
    for (usize i = 0; i < trace.GetCount(); ++i)
    {
        const backend::VulkanSynchronizationRecord& record = trace.GetRecord(i);
        const bool isCounter =
            record.Object == backend::VulkanSynchronizationObject::Buffer && record.Slot == counterSlot;
        if (record.Site == Site::GraphFinal && isCounter == true && record.Execution == firstExecution)
        {
            hasFinalCounter = true;
            CHECK(record.Size == VK_WHOLE_SIZE);
            CHECK(record.Range == (isTrial == true ? Outcome::WholeUse : Outcome::Control));
        }
        if (record.Site == Site::BufferReadback && isCounter == true && record.CopySize == counterBytes)
        {
            // A whole-buffer dependency orders the copy of the declared bytes.
            hasCounterReadback = true;
            CHECK(record.Submission == 0);
            CHECK(record.Size == VK_WHOLE_SIZE);
            CHECK(record.CopyOffset == 0);
            CHECK((record.DestinationAccess & VK_ACCESS_TRANSFER_READ_BIT) == VK_ACCESS_TRANSFER_READ_BIT);
            CHECK((record.SourceStages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        if (record.Site == Site::BufferReadback && record.Object == backend::VulkanSynchronizationObject::StagingBuffer)
        {
            hasStagingReadback = true;
            CHECK(record.DestinationStages == VK_PIPELINE_STAGE_HOST_BIT);
        }
        if ((record.Site == Site::GraphPass || record.Site == Site::GraphFinal) &&
            record.Object == backend::VulkanSynchronizationObject::ColorImage)
        {
            hasGraphImage = true;
            CHECK(record.Size == 0);
            CHECK(record.Subresources.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
            CHECK(record.Subresources.levelCount == 1);
            CHECK(record.Subresources.layerCount == 1);
        }
        if (record.Site == Site::TextureTransfer && record.Object == backend::VulkanSynchronizationObject::ColorImage &&
            record.NewLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            hasImageReadback = true;
            CHECK(record.Subresources.levelCount == 1);
        }
    }
    CHECK(hasFinalCounter == true);
    CHECK(hasCounterReadback == true);
    CHECK(hasStagingReadback == true);
    CHECK(hasGraphImage == true);
    CHECK(hasImageReadback == true);

    // The second graph starts from the updated whole-buffer state, never a first-graph snapshot.
    const backend::VulkanSynchronizationRecord* revalidatedOrNull = nullptr;
    for (usize i = 0; i < trace.GetCount(); ++i)
    {
        const backend::VulkanSynchronizationRecord& record = trace.GetRecord(i);
        if (record.Site == Site::GraphPass && record.Object == backend::VulkanSynchronizationObject::Buffer &&
            record.Slot == counterSlot && record.Execution != firstExecution)
        {
            revalidatedOrNull = &record;
        }
    }
    REQUIRE(revalidatedOrNull != nullptr);
    CHECK(revalidatedOrNull->Submission != firstSubmission);
    CHECK((revalidatedOrNull->SourceStages & VK_PIPELINE_STAGE_TRANSFER_BIT) == VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK((revalidatedOrNull->SourceAccess & VK_ACCESS_TRANSFER_WRITE_BIT) == VK_ACCESS_TRANSFER_WRITE_BIT);
    if (isTrial == true)
    {
        CHECK(revalidatedOrNull->Range == Outcome::Narrowed);
        CHECK(revalidatedOrNull->Offset == stride);
    }

    for (rhi::ComputeDispatch& dispatch : advances)
    {
        REQUIRE(rhi::Destroy(device, dispatch.Pipeline) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::Destroy(device, dispatch.Bindings) == rhi::RasterStatus::Ready);
    }
    REQUIRE(rhi::Destroy(device, prepare.Pipeline) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(device, prepare.Bindings) == rhi::RasterStatus::Ready);
}
} // namespace

TEST_CASE("Vulkan buffer-range trial matches the whole-buffer control with narrower dependencies",
          "[rhi][gpu][vulkan][synchronization-trial]")
{
    const DeviceOwner owner;
    ScenarioResult control;
    ScenarioResult trial;
    RunScenario(owner, false, control);
    RunScenario(owner, true, trial);

    rhi::ComputeCapabilities capabilities;
    REQUIRE(rhi::GetComputeCapabilities(owner.Device, capabilities) == rhi::RasterStatus::Ready);
    const usize stride =
        capabilities.StorageOffsetAlignment > sizeof(uint32) ? capabilities.StorageOffsetAlignment : sizeof(uint32);
    for (usize i = 0; i < COUNTER_COUNT; ++i)
    {
        uint32 counter = 0;
        std::memcpy(&counter, control.CountersAfterFirstGraph.GetData() + i * stride, sizeof(counter));
        CAPTURE(i);
        CHECK(counter == COUNTERS_AFTER_FIRST_GRAPH[i]);
    }
    REQUIRE(trial.CountersAfterFirstGraph.GetSize() == control.CountersAfterFirstGraph.GetSize());
    CHECK(std::memcmp(trial.CountersAfterFirstGraph.GetData(),
                      control.CountersAfterFirstGraph.GetData(),
                      control.CountersAfterFirstGraph.GetSize()) == 0);
    CHECK(control.CounterAfterSecondGraph == 1);
    CHECK(trial.CounterAfterSecondGraph == 1);
    const StaticArray<uint8, CHANNELS> green{0, 255, 0, 255};
    for (usize index = 0; index < PIXEL_BYTES; ++index)
    {
        if (control.Pixels[index] != green[index % CHANNELS])
        {
            CAPTURE(index);
            FAIL("Indirect triangle did not cover the control image");
        }
    }
    CHECK(std::memcmp(trial.Pixels.GetData(), control.Pixels.GetData(), PIXEL_BYTES) == 0);

    // Evidence only: lavapipe timings do not establish a hardware speedup or adoption.
    WARN("R13A control encode_cpu_ns=" << control.EncodeCpu.Nanoseconds
                                       << " gpu_state=" << static_cast<uint32>(control.Timings.State)
                                       << " gpu_passes=" << control.Timings.Count
                                       << " gpu_ns=" << SumAvailableGpuNanoseconds(control.Timings));
    WARN("R13A trial encode_cpu_ns=" << trial.EncodeCpu.Nanoseconds
                                     << " gpu_state=" << static_cast<uint32>(trial.Timings.State) << " gpu_passes="
                                     << trial.Timings.Count << " gpu_ns=" << SumAvailableGpuNanoseconds(trial.Timings));
}
