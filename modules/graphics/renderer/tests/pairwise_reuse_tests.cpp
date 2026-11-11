#include "pairwise_reuse.h"
#include <cstring>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

// Native execution of stochastic pairwise MIS against an independent finite-domain oracle.
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

// Independent CPU oracle for the declared three-element integral. Enumerate all
// 27 source-sample triples and deterministic defensive pairwise weights (Eq. 11).
// The GPU test below uses stochastic weights and the production CDF sampler.
TEST_CASE("Defensive pairwise CPU oracle partitions the complete finite domain", "[renderer][pairwise-reuse]")
{
    constexpr usize DOMAIN_SIZE = 3;
    constexpr usize SOURCE_COUNT = 3;
    const StaticArray<StaticArray<float64, DOMAIN_SIZE>, SOURCE_COUNT> targets{{
        {{1, 2, 4}},
        {{4, .001, 1}},
        {{0, 3, 2}},
    }};
    const StaticArray<float64, DOMAIN_SIZE> integrand{{2, 0, 5}};
    const StaticArray<float64, SOURCE_COUNT> confidence{{2, 3, 5}};
    const StaticArray<uint32, SOURCE_COUNT> drawCounts{{1, 3, 8}};
    for (const uint32 draws : drawCounts)
    {
        const float64 scale = draws < 7 ? static_cast<float64>(draws) / 7 : 1;
        const float64 confidenceSum = (confidence[1] + confidence[2]) * scale;
        float64 expectation = 0;
        for (usize first = 0; first < DOMAIN_SIZE; ++first)
        {
            for (usize second = 0; second < DOMAIN_SIZE; ++second)
            {
                for (usize third = 0; third < DOMAIN_SIZE; ++third)
                {
                    const StaticArray<usize, SOURCE_COUNT> samples{{first, second, third}};
                    float64 canonicalWeight = confidence[0] / (confidenceSum + confidence[0]);
                    float64 estimate = 0;
                    for (usize source = 1; source < SOURCE_COUNT; ++source)
                    {
                        const float64 neighborConfidence = confidence[source] * scale;
                        canonicalWeight += neighborConfidence / (confidenceSum + confidence[0]) * confidence[0] *
                                           targets[0][first] /
                                           (confidenceSum * targets[source][first] + confidence[0] * targets[0][first]);
                        const usize sample = samples[source];
                        const float64 neighborWeight =
                            confidenceSum / (confidenceSum + confidence[0]) * neighborConfidence *
                            targets[source][sample] /
                            (confidenceSum * targets[source][sample] + confidence[0] * targets[0][sample]);
                        estimate += neighborWeight * integrand[sample] * DOMAIN_SIZE;
                    }
                    estimate += canonicalWeight * integrand[first] * DOMAIN_SIZE;
                    expectation += estimate / (DOMAIN_SIZE * DOMAIN_SIZE * DOMAIN_SIZE);
                }
            }
        }
        CHECK(math::Abs(expectation - 7) < 1e-12);
    }
}

TEST_CASE("Stochastic pairwise reuse preserves the finite-domain integral with sparse sources",
          "[renderer][gpu][pairwise-reuse]")
{
    rhi::Shutdown();
    struct Guard
    {
        LUDUS_INLINE ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    constexpr usize INVOCATION_COUNT = 6912;
    constexpr usize TILE_WIDTH = 8;
    constexpr usize RECORDS_PER_INVOCATION = 1;
    constexpr usize WORDS_PER_RECORD = 4;
    constexpr usize RESULT_BYTES = INVOCATION_COUNT * RECORDS_PER_INVOCATION * WORDS_PER_RECORD * sizeof(uint32);
    constexpr usize CELL_BYTES = INVOCATION_COUNT * TILE_WIDTH * 5 * WORDS_PER_RECORD * sizeof(uint32);
    constexpr usize GROUP_WIDTH = 64;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = CELL_BYTES;
    description.RequiredProfile.Compute.MinStorageRange = CELL_BYTES;
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
    const rhi::RasterShaderDescription shader = ludus::shaders::pairwise_reuse::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, results);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);
    Array<uint8> cellBytes;
    REQUIRE(cellBytes.TryResize(CELL_BYTES, uint8{0}) == true);
    rhi::BufferHandle cells;
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         {
                                             .Role = rhi::BufferRole::Storage,
                                             .Size = CELL_BYTES,
                                         },
                                         {cellBytes.GetData(), cellBytes.GetSize()},
                                         cells)) == true);
    WaitReady(device, cells);
    constexpr usize BINDING_COUNT = 2;
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
        { .Binding = 0, .Buffer = results, .Size = RESULT_BYTES },
        { .Binding = 1, .Buffer = cells, .Size = CELL_BYTES },
    }};
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, {resources.GetData(), resources.GetSize()}, set)) == true);
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
    rhi::GraphVersion cellOriginal;
    rhi::GraphVersion cellVersion;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, cells, cellOriginal) == RasterStatus::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, cellOriginal, cellVersion) == RasterStatus::Ready);
    const StaticArray<rhi::GraphUse, BINDING_COUNT> uses{{
        {version, rhi::GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, rhi::RasterVisibility::Compute},
        {cellVersion, rhi::GraphAccessMode::StorageReadWrite, 0, CELL_BYTES, rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {INVOCATION_COUNT / GROUP_WIDTH, 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "stochastic pairwise MIS";
    pass.Uses = uses.GetData();
    pass.UseCount = BINDING_COUNT;
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
    constexpr usize MODE_COUNT = 3;
    StaticArray<float64, MODE_COUNT> sums{};
    for (usize invocation = 0; invocation < INVOCATION_COUNT; ++invocation)
    {
        StaticArray<uint32, WORDS_PER_RECORD> record{};
        std::memcpy(record.GetData(), bytes.GetData() + invocation * sizeof(record), sizeof(record));
        REQUIRE(record[3] == 0);
        for (usize mode = 0; mode < MODE_COUNT; ++mode)
        {
            float32 value = 0;
            std::memcpy(&value, &record[mode], sizeof(value));
            REQUIRE(math::IsFinite(value) == true);
            sums[mode] += static_cast<float64>(value);
        }
    }
    // Independent exact integral: f = {2, 0, 5} on a three-element domain.
    // All 27 combinations of independent input samples occur equally often.
    constexpr float64 EXACT_INTEGRAL = 7;
    constexpr float64 RELATIVE_MONTE_CARLO_TOLERANCE = 0.025;
    for (usize mode = 0; mode < MODE_COUNT; ++mode)
    {
        const float64 mean = sums[mode] / static_cast<float64>(INVOCATION_COUNT);
        INFO("draw mode=" << mode << ", raw linear mean=" << mean);
        CHECK(math::Abs(mean - EXACT_INTEGRAL) < EXACT_INTEGRAL * RELATIVE_MONTE_CARLO_TOLERANCE);
    }
}
