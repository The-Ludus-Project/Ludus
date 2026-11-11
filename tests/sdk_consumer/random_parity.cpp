#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "random_parity.h"
#include "random_parity_corpus.h"

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#endif

// Installed-SDK fixture for FoundationMath's shader port of addressed random
// v1. It runs the corpus shared with the native renderer test
// (modules/graphics/renderer/tests/gpu_random_tests.cpp) through the RHI compute
// path and compares every result word with the CPU reference compiled into this
// executable; in the browser that reference is WebAssembly and the GPU is
// WebGPU. See docs/architecture/randomness.md#gpu-addressed-sampling.
namespace
{
using namespace ludus::foundation;
using namespace ludus::tests::random_parity;
namespace rhi = ludus::graphics::rhi;
constexpr logging::LogCategory LOG_RANDOM_PARITY{"Random-Parity-Fixture"};
constexpr usize BINDING_COUNT = 2;
// Per-request readback limit; well below the default 256 KiB transfer profile.
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
#if !defined(LUDUS_PLATFORM_WEB)
constexpr uint64 NATIVE_TIMEOUT_NANOSECONDS = 60 * time::NANOSECONDS_PER_SECOND;
#endif

enum class Stage : uint32
{
    CreateResources = 0,
    CreateDispatch = 1,
    CompileGraph = 2,
    Execute = 3,
    ReadBack = 4,
};

ParityCorpus gCorpus;
Array<math::RandomBlock> gActual;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::BufferHandle gCaseBuffer;
rhi::BufferHandle gResultBuffer;
rhi::RasterShaderHandle gShader;
rhi::BindingLayoutHandle gLayout;
rhi::BindingSetHandle gSet;
rhi::ComputePipelineHandle gPipeline;
rhi::OrderedGraph gGraph;
rhi::ReadbackTicket gReadback;
usize gReadbackOffset = 0;
Stage gStage = Stage::CreateResources;
usize gMismatchCount = 0;
bool gHasFailed = false;
bool gIsDone = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(void, Report, (uint32 stage, int32 hasFailed, int32 isDone, uint32 caseCount, uint32 mismatchCount), {
    const node = document.querySelector('#status');
    node.dataset.state = hasFailed ? 'failed' : isDone ? 'passed' : 'loading';
    node.dataset.stage = stage;
    node.dataset.cases = caseCount;
    node.dataset.mismatches = mismatchCount;
});
// clang-format on
#else
void Report(uint32, int32, int32, uint32, uint32) noexcept {}
#endif
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
void Stop(bool hasFailed) noexcept
{
    gHasFailed = hasFailed;
    gIsDone = true;
    LUDUS_LOG_INFO(LOG_RANDOM_PARITY,
                   "Random parity passed {}; stage {}; cases {}; mismatches {}",
                   hasFailed == false,
                   static_cast<uint32>(gStage),
                   gCorpus.GetCases().GetSize(),
                   gMismatchCount);
    Report(static_cast<uint32>(gStage),
           hasFailed == true ? 1 : 0,
           1,
           static_cast<uint32>(gCorpus.GetCases().GetSize()),
           static_cast<uint32>(gMismatchCount));
    rhi::Shutdown();
    (void)logging::LogSystem::Shutdown();
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
}
template <typename T>
bool IsReady(T handle) noexcept
{
    const rhi::RasterStatus status = rhi::GetStatus(gDevice, handle);
    if (IsAccepted(status) == false)
    {
        Stop(true);
    }
    return status == rhi::RasterStatus::Ready;
}
bool CreateResources() noexcept
{
    const usize caseBytes = gCorpus.GetCaseBytes();
    const usize resultBytes = gCorpus.GetResultBytes();
    Array<uint8> zeros;
    if (zeros.TryResize(resultBytes, uint8{0}) == false)
    {
        return false;
    }
    const ludus::foundation::BorrowedView<const uint8> caseSource{
        reinterpret_cast<const uint8*>(gCorpus.GetCases().GetData()),
        caseBytes};
    const ludus::foundation::BorrowedView<const uint8> resultSource{zeros.GetData(), zeros.GetSize()};
    const rhi::RasterShaderDescription shader = ludus::shaders::random_parity::Compute();
    return IsAccepted(rhi::CreateBuffer(gDevice,
                                        { .Role = rhi::BufferRole::Storage, .Size = caseBytes },
                                        caseSource,
                                        gCaseBuffer)) == true &&
           IsAccepted(rhi::CreateBuffer(gDevice,
                                        { .Role = rhi::BufferRole::Storage, .Size = resultBytes },
                                        resultSource,
                                        gResultBuffer)) == true &&
           IsAccepted(rhi::CreateRasterShader(gDevice, shader, gShader)) == true &&
           IsAccepted(rhi::CreateBindingLayout(gDevice, shader.Bindings, gLayout)) == true;
}
bool CreateDispatch() noexcept
{
    const StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources = {{
        { .Binding = 0, .Buffer = gCaseBuffer, .Size = gCorpus.GetCaseBytes() },
        { .Binding = 1, .Buffer = gResultBuffer, .Size = gCorpus.GetResultBytes() },
    }};
    return IsAccepted(rhi::CreateBindingSet(gDevice, gLayout, resources, gSet)) == true &&
           IsAccepted(rhi::CreateComputePipeline(gDevice, { .Shader = gShader, .Layout = gLayout }, gPipeline)) == true;
}
bool CompileGraph() noexcept
{
    rhi::GraphVersion caseVersion;
    rhi::GraphVersion initialResults;
    rhi::GraphVersion results;
    if (rhi::CreateOrderedGraph(gDevice, gGraph) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphBuffer(gDevice, gGraph, gCaseBuffer, caseVersion) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphBuffer(gDevice, gGraph, gResultBuffer, initialResults) != rhi::RasterStatus::Ready ||
        rhi::NextGraphVersion(gDevice, gGraph, initialResults, results) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    const StaticArray<rhi::GraphUse, BINDING_COUNT> uses = {{
        {caseVersion, rhi::GraphAccessMode::StorageRead, 0, gCorpus.GetCaseBytes(), rhi::RasterVisibility::Compute},
        {results, rhi::GraphAccessMode::StorageReadWrite, 0, gCorpus.GetResultBytes(), rhi::RasterVisibility::Compute},
    }};
    const rhi::ComputeDispatch dispatch
    {
        .Pipeline = gPipeline,
        .Bindings = gSet,
        .Groups = {gCorpus.GetGroupCount(), 1, 1},
    };
    const rhi::GraphPassDescription pass
    {
        .Name = "addressed random parity",
        .Attachment = {},
        .Uses = uses.GetData(),
        .UseCount = BINDING_COUNT,
        .SideEffect = true,
        .Dispatch = &dispatch,
    };
    rhi::GraphReport report;
    const rhi::RasterStatus added = rhi::AddGraphPass(gDevice, gGraph, pass);
    const rhi::RasterStatus compiled =
        added == rhi::RasterStatus::Ready ? rhi::CompileOrderedGraph(gDevice, gGraph, report) : added;
    if (compiled != rhi::RasterStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_RANDOM_PARITY,
                        "Graph add {} compile {}; error pass {} resource {}",
                        static_cast<uint32>(added),
                        static_cast<uint32>(compiled),
                        report.ErrorPass,
                        report.ErrorResource);
    }
    return compiled == rhi::RasterStatus::Ready;
}
usize GetReadbackChunkBytes() noexcept
{
    const usize remaining = gCorpus.GetResultBytes() - gReadbackOffset;
    return remaining < READBACK_CHUNK_BYTES ? remaining : READBACK_CHUNK_BYTES;
}
bool RequestReadbackChunk() noexcept
{
    return rhi::RequestBufferReadback(gDevice, gResultBuffer, gReadbackOffset, GetReadbackChunkBytes(), gReadback) ==
           rhi::RasterStatus::Pending;
}
void CompareAndStop() noexcept
{
    ParityComparison comparison;
    if (TryCompareResults(gCorpus,
                          BorrowedView<const math::RandomBlock>{gActual.GetData(), gActual.GetSize()},
                          comparison) == false)
    {
        Stop(true);
        return;
    }
    gMismatchCount = comparison.MismatchCount;
    for (usize i = 0; i < comparison.MismatchCount && i < MAX_REPORTED_MISMATCHES; ++i)
    {
        const ParityMismatch& mismatch = comparison.Reported[i];
        LUDUS_LOG_ERROR(LOG_RANDOM_PARITY,
                        "Mismatch case {} kind {} lane {}: GPU {} expected {}",
                        mismatch.Index,
                        mismatch.Kind,
                        mismatch.Lane,
                        mismatch.Actual,
                        mismatch.Expected);
    }
    Stop(comparison.MismatchCount != 0);
}
void Tick() noexcept
{
    if (gIsDone == true)
    {
        return;
    }
    rhi::DeviceInfo info;
    const rhi::DeviceStatus status = rhi::GetDeviceInfo(gDevice, info);
    if (status != rhi::DeviceStatus::Ready)
    {
        if (status != rhi::DeviceStatus::Pending)
        {
            Stop(true);
        }
        return;
    }
    if (rhi::PollLifetime(gDevice) != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return;
    }
    if (gStage == Stage::CreateResources)
    {
        if (CreateResources() == false)
        {
            Stop(true);
            return;
        }
        gStage = Stage::CreateDispatch;
    }
    if (gStage == Stage::CreateDispatch)
    {
        if (IsReady(gCaseBuffer) == false || IsReady(gResultBuffer) == false || IsReady(gShader) == false ||
            IsReady(gLayout) == false)
        {
            return;
        }
        if (CreateDispatch() == false)
        {
            Stop(true);
            return;
        }
        gStage = Stage::CompileGraph;
    }
    if (gStage == Stage::CompileGraph)
    {
        if (IsReady(gSet) == false || IsReady(gPipeline) == false)
        {
            return;
        }
        if (rhi::SetFrameTarget(gDevice, gSurface, {16, 16, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready ||
            CompileGraph() == false)
        {
            Stop(true);
            return;
        }
        gStage = Stage::Execute;
    }
    if (gStage == Stage::Execute)
    {
        rhi::SubmissionToken completion;
        const rhi::RasterStatus executed = rhi::ExecuteOrderedGraph(gDevice, gSurface, gGraph, completion);
        if (executed == rhi::RasterStatus::NotReady)
        {
            return;
        }
        if (executed != rhi::RasterStatus::Ready || RequestReadbackChunk() == false)
        {
            Stop(true);
            return;
        }
        gStage = Stage::ReadBack;
    }
    if (gStage == Stage::ReadBack)
    {
        const usize size = GetReadbackChunkBytes();
        uint8* destination = reinterpret_cast<uint8*>(gActual.GetData()) + gReadbackOffset;
        const rhi::RasterStatus copied = rhi::CopyReadback(gDevice, gReadback, destination, size);
        if (copied == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (copied != rhi::RasterStatus::Ready || rhi::Release(gDevice, gReadback) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gReadbackOffset += size;
        if (gReadbackOffset < gCorpus.GetResultBytes())
        {
            if (RequestReadbackChunk() == false)
            {
                Stop(true);
            }
            return;
        }
        CompareAndStop();
    }
}
} // namespace
int main()
{
    (void)logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} });
    if (TryBuildCorpus(gCorpus) == false || gActual.TryResize(gCorpus.GetCases().GetSize()) == false)
    {
        LUDUS_LOG_ERROR(LOG_RANDOM_PARITY, "The CPU reference corpus could not be built");
        Stop(true);
        return 1;
    }
    rhi::WindowInfo window{ .Width = 16, .Height = 16 };
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo create;
    create.Width = 16;
    create.Height = 16;
    create.CanvasSelector = "#canvas";
    create.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(create, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.RequiredProfile.Raster.MinBufferSize = gCorpus.GetCaseBytes();
    description.RequiredProfile.Raster.MinBindings = BINDING_COUNT;
    description.RequiredProfile.Compute.MinStorageRange = gCorpus.GetCaseBytes();
    description.RequiredProfile.Compute.MinStorageBindings = BINDING_COUNT;
    const rhi::DeviceStatus created = rhi::CreateDevice({}, window, description, gDevice, gSurface);
    if (created != rhi::DeviceStatus::Ready && created != rhi::DeviceStatus::Pending)
    {
        LUDUS_LOG_ERROR(LOG_RANDOM_PARITY, "Device creation failed: {}", static_cast<uint32>(created));
        Stop(true);
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const uint64 started = time::NowTicks();
    while (gIsDone == false && time::NowTicks() - started < NATIVE_TIMEOUT_NANOSECONDS)
    {
        Tick();
    }
    if (gIsDone == false)
    {
        Stop(true);
    }
#endif
    return gHasFailed == true ? 1 : 0;
}
