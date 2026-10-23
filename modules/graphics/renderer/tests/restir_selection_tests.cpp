#include "restir_selection.h"
#include <cstring>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

// Native execution of the production batched emitter selector against known weights.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;

template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    RasterStatus status = rhi::GetStatus(device, handle);
    while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == RasterStatus::Ready);
}

bool IsAccepted(RasterStatus status) noexcept
{
    return status == RasterStatus::Ready || status == RasterStatus::Pending;
}

} // namespace

TEST_CASE("Batched emitter selection preserves PMFs, independent draws and shared weight evaluation",
          "[renderer][gpu][restir-selection]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    constexpr usize INVOCATION_COUNT = 5120;
    constexpr usize CANDIDATE_COUNT = 8;
    constexpr usize RECORDS_PER_INVOCATION = CANDIDATE_COUNT + 1;
    constexpr usize WORDS_PER_RECORD = 4;
    constexpr usize RESULT_BYTES = INVOCATION_COUNT * RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
    constexpr usize GROUP_WIDTH = 64;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Compute.MinStorageRange = RESULT_BYTES;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No native GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(RESULT_BYTES, uint8{0}) == true);
    rhi::BufferHandle results;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = RESULT_BYTES },
                                         {bytes.GetData(), bytes.GetSize()},
                                         results)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::restir_selection::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    const rhi::RasterBindingResource resource{ .Binding = 0, .Buffer = results, .Size = RESULT_BYTES };
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, {&resource, 1}, set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);
    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == RasterStatus::Ready);
    rhi::GraphVersion original;
    rhi::GraphVersion version;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, results, original) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, original, version) == RasterStatus::Ready);
    const rhi::GraphUse use{version,
                            rhi::GraphAccessMode::StorageReadWrite,
                            0,
                            RESULT_BYTES,
                            rhi::RasterVisibility::Compute};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {INVOCATION_COUNT / GROUP_WIDTH, 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "batched emitter sampling";
    pass.Uses = &use;
    pass.UseCount = 1;
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == RasterStatus::Ready);
    rhi::GraphReport graphReport;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, graphReport) == RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    RasterStatus executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    const uint64 beginning = time::NowTicks();
    while (executed == RasterStatus::NotReady && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == RasterStatus::Ready);
    WaitReady(device, completion);
    for (usize offset = 0; offset < RESULT_BYTES; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = RESULT_BYTES - offset < READBACK_CHUNK_BYTES ? RESULT_BYTES - offset : READBACK_CHUNK_BYTES;
        rhi::ReadbackTicket ticket;
        RasterStatus status = rhi::RequestBufferReadback(device, results, offset, size, ticket);
        const uint64 start = time::NowTicks();
        while (status == RasterStatus::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
        {
            REQUIRE(rhi::PollLifetime(device) == RasterStatus::Ready);
            status = rhi::GetStatus(device, ticket);
        }
        REQUIRE(status == RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, bytes.GetData() + offset, size) == RasterStatus::Ready);
        REQUIRE(rhi::Release(device, ticket) == RasterStatus::Ready);
    }
    constexpr usize EMITTER_COUNT = 5;
    StaticArray<uint32, EMITTER_COUNT> histogram{};
    uint32 matchingFirstPair = 0;
    constexpr usize DRAW_COUNT = INVOCATION_COUNT / 5 * 2 * CANDIDATE_COUNT;
    constexpr float32 TOTAL_WEIGHT = 11;
    for (usize invocation = 0; invocation < INVOCATION_COUNT; ++invocation)
    {
        StaticArray<uint32, WORDS_PER_RECORD> summary{};
        const usize base = invocation * RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
        std::memcpy(summary.GetData(), bytes.GetData() + base, sizeof(summary));
        const usize scenario = invocation % 5;
        REQUIRE(summary[0] == (scenario == 0 || scenario == 4 ? 1 : 0));
        REQUIRE(summary[1] == (scenario == 2 || scenario == 3 ? 1 : 0));
        // Five weights are evaluated once, rather than five times per candidate.
        REQUIRE(summary[3] == (scenario == 2 || scenario == 3 ? 1 : scenario == 4 ? 17 : EMITTER_COUNT));
        if (scenario != 0 && scenario != 4)
        {
            continue;
        }
        REQUIRE(summary[2] == (scenario == 4 ? 3 : 2) * CANDIDATE_COUNT);
        uint32 firstEmitter = 0;
        for (usize candidate = 0; candidate < CANDIDATE_COUNT; ++candidate)
        {
            StaticArray<uint32, WORDS_PER_RECORD> selection{};
            std::memcpy(selection.GetData(),
                        bytes.GetData() + base + (candidate + 1) * sizeof(selection),
                        sizeof(selection));
            const uint32 emitter = selection[0];
            REQUIRE(emitter < EMITTER_COUNT);
            REQUIRE((emitter == 1 || emitter == 2 || emitter == 4));
            const float32 weight = emitter == 1 ? 1.0F : emitter == 2 ? 3.0F : 7.0F;
            float32 probability = 0;
            std::memcpy(&probability, &selection[1], sizeof(probability));
            CHECK(math::Abs(probability - weight / TOTAL_WEIGHT) < 1e-6F);
            if (candidate == 0)
            {
                firstEmitter = emitter;
            }
            if (candidate == 1 && emitter == firstEmitter)
            {
                ++matchingFirstPair;
            }
            ++histogram[emitter];
        }
    }
    // Independent selections agree with probability sum(p_i^2) = (1+9+49)/121.
    // Reusing one selected emitter for every candidate would fail this check.
    constexpr float32 EXPECTED_PAIR_MATCHES = static_cast<float32>(INVOCATION_COUNT / 5 * 2) * 59.0F / 121.0F;
    constexpr float32 PAIR_COUNT_TOLERANCE = 100;
    CHECK(math::Abs(static_cast<float32>(matchingFirstPair) - EXPECTED_PAIR_MATCHES) < PAIR_COUNT_TOLERANCE);
    // Independent binomial margins, deliberately wider than six standard deviations.
    constexpr float32 COUNT_TOLERANCE = 300;
    for (usize emitter : {usize{1}, usize{2}, usize{4}})
    {
        const float32 weight = emitter == 1 ? 1.0F : emitter == 2 ? 3.0F : 7.0F;
        CHECK(math::Abs(static_cast<float32>(histogram[emitter]) -
                        static_cast<float32>(DRAW_COUNT) * weight / TOTAL_WEIGHT) < COUNT_TOLERANCE);
    }
}
