#include "random_parity.h"
#include "random_parity_corpus.h"
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

// GPU evidence for FoundationMath's shader port of addressed random v1
// (modules/foundation/math/shaders/math.slang); see
// docs/architecture/randomness.md#gpu-addressed-sampling. The case corpus and
// comparison are shared with the WebGPU browser fixture
// (tests/sdk_consumer/random_parity.cpp) through random_parity_corpus.h.
namespace
{
using namespace ludus::foundation;
using namespace ludus::tests::random_parity;
namespace math = ludus::foundation::math;
namespace rhi = ludus::graphics::rhi;
using S = rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;

template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    S status = rhi::GetStatus(device, handle);
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == S::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == S::Ready);
}

bool IsAccepted(S status) noexcept
{
    return status == S::Ready || status == S::Pending;
}

} // namespace

TEST_CASE("GPU addressed random matches the CPU v1 reference and Random123 known answers", "[renderer][gpu][random]")
{
    rhi::Shutdown();
    struct Guard
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    ParityCorpus corpus;
    REQUIRE(TryBuildCorpus(corpus) == true);
    const Array<ParityCase>& cases = corpus.GetCases();
    const usize caseBytes = corpus.GetCaseBytes();
    const usize resultBytes = corpus.GetResultBytes();

    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = caseBytes;
    description.RequiredProfile.Compute.MinStorageRange = caseBytes;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);

    rhi::BufferHandle caseBuffer;
    rhi::BufferHandle resultBuffer;
    Array<uint8> zeros;
    REQUIRE(zeros.TryResize(resultBytes, uint8{0}) == true);
    const ludus::foundation::BorrowedView<const uint8> caseSource{reinterpret_cast<const uint8*>(cases.GetData()),
                                                                  caseBytes};
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         {
                                             .Role = rhi::BufferRole::Storage,
                                             .Size = caseBytes,
                                         },
                                         caseSource,
                                         caseBuffer)) == true);
    REQUIRE(IsAccepted(rhi::CreateBuffer(device,
                                         { .Role = rhi::BufferRole::Storage, .Size = resultBytes },
                                         ludus::foundation::BorrowedView<const uint8>{zeros.GetData(), zeros.GetSize()},
                                         resultBuffer)) == true);
    const rhi::RasterShaderDescription shader = ludus::shaders::random_parity::Compute();
    rhi::RasterShaderHandle shaderHandle;
    rhi::BindingLayoutHandle layout;
    REQUIRE(IsAccepted(rhi::CreateRasterShader(device, shader, shaderHandle)) == true);
    REQUIRE(IsAccepted(rhi::CreateBindingLayout(device, shader.Bindings, layout)) == true);
    WaitReady(device, caseBuffer);
    WaitReady(device, resultBuffer);
    WaitReady(device, shaderHandle);
    WaitReady(device, layout);

    const rhi::RasterBindingResource resources[] = {
        { .Binding = 0, .Buffer = caseBuffer, .Size = caseBytes },
        { .Binding = 1, .Buffer = resultBuffer, .Size = resultBytes },
    };
    rhi::BindingSetHandle set;
    rhi::ComputePipelineHandle pipeline;
    REQUIRE(IsAccepted(rhi::CreateBindingSet(device, layout, resources, set)) == true);
    REQUIRE(IsAccepted(rhi::CreateComputePipeline(device, { .Shader = shaderHandle, .Layout = layout }, pipeline)) ==
            true);
    WaitReady(device, set);
    WaitReady(device, pipeline);

    rhi::OrderedGraph graph;
    REQUIRE(rhi::CreateOrderedGraph(device, graph) == S::Ready);
    rhi::GraphVersion caseVersion;
    rhi::GraphVersion initialResults;
    rhi::GraphVersion results;
    REQUIRE(rhi::ImportGraphBuffer(device, graph, caseBuffer, caseVersion) == S::Ready);
    REQUIRE(rhi::ImportGraphBuffer(device, graph, resultBuffer, initialResults) == S::Ready);
    REQUIRE(rhi::NextGraphVersion(device, graph, initialResults, results) == S::Ready);
    const rhi::GraphUse uses[] = {
        {caseVersion, rhi::GraphAccessMode::StorageRead, 0, caseBytes, rhi::RasterVisibility::Compute},
        {results, rhi::GraphAccessMode::StorageReadWrite, 0, resultBytes, rhi::RasterVisibility::Compute},
    };
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = pipeline,
        .Bindings = set,
        .Groups = {corpus.GetGroupCount(), 1, 1},
    };
    rhi::GraphPassDescription pass;
    pass.Name = "addressed random parity";
    pass.Uses = uses;
    pass.UseCount = 2;
    pass.SideEffect = true;
    pass.Dispatch = &dispatch;
    REQUIRE(rhi::AddGraphPass(device, graph, pass) == S::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == S::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    const uint64 start = time::NowTicks();
    S executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (executed == S::NotReady && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        executed = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(executed == S::Ready);

    Array<math::RandomBlock> actual;
    REQUIRE(actual.TryResize(cases.GetSize()) == true);
    uint8* actualBytes = reinterpret_cast<uint8*>(actual.GetData());
    for (usize offset = 0; offset < resultBytes; offset += READBACK_CHUNK_BYTES)
    {
        const usize size = resultBytes - offset < READBACK_CHUNK_BYTES ? resultBytes - offset : READBACK_CHUNK_BYTES;
        rhi::ReadbackTicket ticket;
        S readback = rhi::RequestBufferReadback(device, resultBuffer, offset, size, ticket);
        const uint64 readbackStart = time::NowTicks();
        while (readback == S::Pending && time::NowTicks() - readbackStart < TIMEOUT_NANOSECONDS)
        {
            REQUIRE(rhi::PollLifetime(device) == S::Ready);
            readback = rhi::GetStatus(device, ticket);
        }
        REQUIRE(readback == S::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, actualBytes + offset, size) == S::Ready);
        REQUIRE(rhi::Release(device, ticket) == S::Ready);
    }

    ParityComparison comparison;
    REQUIRE(TryCompareResults(corpus,
                              BorrowedView<const math::RandomBlock>{actual.GetData(), actual.GetSize()},
                              comparison) == true);
    for (usize i = 0; i < comparison.MismatchCount && i < MAX_REPORTED_MISMATCHES; ++i)
    {
        const ParityMismatch& mismatch = comparison.Reported[i];
        CAPTURE(mismatch.Index, mismatch.Kind, mismatch.Lane);
        CHECK(mismatch.Actual == mismatch.Expected);
    }
    REQUIRE(comparison.MismatchCount == 0);
}
