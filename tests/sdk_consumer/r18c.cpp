#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "r18c.h"

#include <cstring>

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#endif
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
constexpr logging::LogCategory LOG_R18C{"R18c-Fixture"};
constexpr usize BINDING_COUNT = 9;
constexpr usize LARGE_BYTES = usize{64} * 1024 * 1024 + 256;
constexpr usize RANGE_BYTES = usize{16} * 1024 * 1024 + 256;
constexpr usize RANGE_OFFSET = LARGE_BYTES - RANGE_BYTES;
constexpr uint32 EXPECTED_VALUE = 20;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
StaticArray<rhi::BufferHandle, BINDING_COUNT> gBuffers{};
rhi::RasterShaderHandle gShader;
rhi::BindingLayoutHandle gLayout;
rhi::BindingSetHandle gSet;
rhi::ComputePipelineHandle gPipeline;
rhi::OrderedGraph gGraph;
rhi::ReadbackTicket gReadback;
uint32 gStage = 0;
bool gHasFailed = false;
bool gIsDone = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(void, Report, (uint32 stage, int32 hasFailed, int32 isDone), {
    const node = document.querySelector('#status');
    node.dataset.state = hasFailed ? 'failed' : isDone ? 'passed' : 'loading';
    node.dataset.stage = stage;
});
// clang-format on
#else
void Report(uint32, int32, int32) noexcept {}
#endif
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
void Stop(bool hasFailed) noexcept
{
    gHasFailed = hasFailed;
    gIsDone = true;
    LUDUS_LOG_INFO(LOG_R18C, "R18c passed {}; stage {}", hasFailed == false, gStage);
    Report(gStage, hasFailed ? 1 : 0, 1);
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
bool PrepareBuffers() noexcept
{
    Array<uint8> bytes;
    if (bytes.TryResize(LARGE_BYTES) == false)
    {
        return false;
    }
    std::memset(bytes.GetData(), 0, bytes.GetSize());
    const uint32 primary = 7;
    std::memcpy(bytes.GetData() + LARGE_BYTES - sizeof(primary), &primary, sizeof(primary));
    if (IsAccepted(rhi::CreateBuffer(gDevice,
                                     {
                                         .Role = rhi::BufferRole::Storage,
                                         .Size = LARGE_BYTES,
                                     },
                                     {bytes.GetData(), bytes.GetSize()},
                                     gBuffers[0])) == false)
    {
        return false;
    }
    constexpr usize UNIFORM_WORDS = 4;
    for (usize binding = 1; binding < BINDING_COUNT; ++binding)
    {
        StaticArray<uint32, UNIFORM_WORDS> words{};
        words[0] = binding < 4 ? static_cast<uint32>(binding + 1) : binding == 4 ? 0U : 1U;
        if (binding == 5)
        {
            words[0] = static_cast<uint32>(RANGE_BYTES / sizeof(uint32) - 1);
            words[1] = 1;
        }
        const usize size = binding < 5 ? sizeof(uint32) : sizeof(words);
        const ludus::foundation::BorrowedView<const uint8> initial{reinterpret_cast<const uint8*>(words.GetData()),
                                                                   size};
        if (IsAccepted(rhi::CreateBuffer(
                gDevice,
                { .Role = binding < 5 ? rhi::BufferRole::Storage : rhi::BufferRole::Uniform, .Size = size },
                initial,
                gBuffers[binding])) == false)
        {
            return false;
        }
    }
    const rhi::RasterShaderDescription shader = ludus::shaders::r18c::Compute();
    return IsAccepted(rhi::CreateRasterShader(gDevice, shader, gShader)) == true &&
           IsAccepted(rhi::CreateBindingLayout(gDevice, shader.Bindings, gLayout)) == true;
}
bool PrepareDispatch() noexcept
{
    StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{};
    for (usize binding = 0; binding < BINDING_COUNT; ++binding)
    {
        resources[binding].Binding = static_cast<uint32>(binding);
        resources[binding].Buffer = gBuffers[binding];
        resources[binding].Size = binding == 0 ? RANGE_BYTES : binding < 5 ? 4U : 16U;
        resources[binding].Offset = binding == 0 ? RANGE_OFFSET : 0;
    }
    return IsAccepted(rhi::CreateBindingSet(gDevice, gLayout, {resources.GetData(), resources.GetSize()}, gSet)) ==
               true &&
           IsAccepted(rhi::CreateComputePipeline(gDevice, { .Shader = gShader, .Layout = gLayout }, gPipeline)) == true;
}
bool PrepareGraph() noexcept
{
    StaticArray<rhi::GraphUse, BINDING_COUNT> uses{};
    if (rhi::CreateOrderedGraph(gDevice, gGraph) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    for (usize binding = 0; binding < BINDING_COUNT; ++binding)
    {
        rhi::GraphVersion input;
        if (rhi::ImportGraphBuffer(gDevice, gGraph, gBuffers[binding], input) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        if (binding == 4 &&
            rhi::NextGraphVersion(gDevice, gGraph, input, uses[binding].Resource) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        if (binding != 4)
        {
            uses[binding].Resource = input;
        }
        uses[binding].Access = binding == 4  ? rhi::GraphAccessMode::StorageReadWrite
                               : binding < 5 ? rhi::GraphAccessMode::StorageRead
                                             : rhi::GraphAccessMode::Uniform;
        uses[binding].Offset = binding == 0 ? RANGE_OFFSET : 0;
        uses[binding].Size = binding == 0 ? RANGE_BYTES : binding < 5 ? 4U : 16U;
        uses[binding].Visibility = rhi::RasterVisibility::Compute;
    }
    const rhi::ComputeDispatch dispatch{ .Pipeline = gPipeline, .Bindings = gSet };
    const rhi::GraphPassDescription pass
    {
        .Name = "large storage tail and nine bindings",
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
        LUDUS_LOG_ERROR(LOG_R18C,
                        "Graph add {} compile {}; error pass {} resource {}",
                        static_cast<uint32>(added),
                        static_cast<uint32>(compiled),
                        report.ErrorPass,
                        report.ErrorResource);
    }
    return compiled == rhi::RasterStatus::Ready;
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
    if (gStage == 0)
    {
        if (PrepareBuffers() == false)
        {
            Stop(true);
            return;
        }
        gStage = 1;
    }
    if (gStage == 1)
    {
        for (const rhi::BufferHandle buffer : gBuffers)
        {
            if (IsReady(buffer) == false)
            {
                return;
            }
        }
        if (IsReady(gShader) == false || IsReady(gLayout) == false)
        {
            return;
        }
        if (PrepareDispatch() == false)
        {
            Stop(true);
            return;
        }
        gStage = 2;
    }
    if (gStage == 2)
    {
        if (IsReady(gSet) == false || IsReady(gPipeline) == false)
        {
            return;
        }
        if (rhi::SetFrameTarget(gDevice, gSurface, {16, 16, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready ||
            PrepareGraph() == false)
        {
            Stop(true);
            return;
        }
        gStage = 3;
    }
    if (gStage == 3)
    {
        rhi::SubmissionToken completion;
        const rhi::RasterStatus executed = rhi::ExecuteOrderedGraph(gDevice, gSurface, gGraph, completion);
        if (executed == rhi::RasterStatus::NotReady)
        {
            return;
        }
        if (executed != rhi::RasterStatus::Ready ||
            rhi::RequestBufferReadback(gDevice, gBuffers[4], 0, sizeof(uint32), gReadback) !=
                rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 4;
    }
    if (gStage == 4)
    {
        uint32 actual = 0;
        const rhi::RasterStatus copied =
            rhi::CopyReadback(gDevice, gReadback, reinterpret_cast<uint8*>(&actual), sizeof(actual));
        if (copied == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (copied != rhi::RasterStatus::Ready || actual != EXPECTED_VALUE)
        {
            Stop(true);
            return;
        }
        LUDUS_LOG_INFO(LOG_R18C,
                       "R18c buffer {} bytes; bound range {} bytes; five storage/nine total bindings; GPU result {}",
                       LARGE_BYTES,
                       RANGE_BYTES,
                       actual);
        Stop(false);
    }
}
} // namespace
int main()
{
    (void)logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} });
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
    description.RequiredProfile.Raster.MinBufferSize = LARGE_BYTES;
    description.RequiredProfile.Raster.MinBindings = BINDING_COUNT;
    description.RequiredProfile.Compute.MinStorageRange = RANGE_BYTES;
    description.RequiredProfile.Compute.MinStorageBindings = 5;
    description.RequiredProfile.Compute.MaxStorageOffsetAlignment = 256;
    description.RequiredProfile.Raster.MinUniformBindings = 4;
    const rhi::DeviceStatus created = rhi::CreateDevice({}, window, description, gDevice, gSurface);
    if (created != rhi::DeviceStatus::Ready && created != rhi::DeviceStatus::Pending)
    {
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const uint64 started = time::NowTicks();
    while (gIsDone == false && time::NowTicks() - started < 30 * time::NANOSECONDS_PER_SECOND)
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
