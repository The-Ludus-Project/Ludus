#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "cull.h"
#include "raster.h"

#include <span>

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#endif
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
constexpr usize OBJECTS = 72;
constexpr ludus::foundation::logging::LogCategory LOG_R6{"R6-Fixture"};
uint32 gStage = 0;
bool gRetry = false;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::BufferHandle gVertices, gInstances, gIndices;
rhi::BufferHandle gBuffers[OBJECTS];
rhi::TextureHandle gTextures[OBJECTS];
rhi::TextureViewHandle gViews[OBJECTS];
rhi::SamplerHandle gSamplers[OBJECTS];
rhi::RasterShaderHandle gShaders[OBJECTS], gFragment, gComputeShader;
rhi::BindingLayoutHandle gLayouts[OBJECTS], gComputeLayout;
rhi::BindingSetHandle gSets[OBJECTS];
rhi::RasterPipelineHandle gPipelines[OBJECTS];
rhi::ComputePipelineHandle gKernels[OBJECTS];
rhi::RasterVertexSlice gSlices[2];
rhi::RasterDraw gDraws[1100];
bool gCreated = false, gViewsCreated = false, gLinked = false, gCompute = false, gDone = false, gFailed = false;
uint32 gFrames = 0;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// Application-owned fixture reporting, outside the RHI API.
// clang-format off
EM_JS(int32, Selection, (), { const b = new URL(location.href).searchParams.get('backend'); return b === 'webgpu' ? 1 : b === 'webgl2' ? 2 : 0; });
EM_JS(int32, Reference, (), { return new URL(location.href).searchParams.get('reference') === '1' ? 1 : 0; });
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(void, Report, (uint32 frames, int32 failed, int32 done, int32 compute, uint32 passes, uint32 draws), {
    const n = document.getElementById('status'); n.dataset.state = failed ? 'failed' : done ? 'passed' : frames ? 'rendering' : 'loading';
    n.dataset.frames = frames; n.dataset.compute = compute; n.dataset.passes = passes; n.dataset.draws = draws;
    n.textContent = n.dataset.state + ' frames=' + frames;
});
// clang-format on
#else
int32 Selection() noexcept
{
    return 0;
}
int32 Reference() noexcept
{
    return 0;
}
int32 Paused() noexcept
{
    return 0;
}
void Report(uint32, int32, int32, int32, uint32, uint32) noexcept {}
#endif
void Stop(bool failed) noexcept
{
    if (failed)
    {
        LUDUS_LOG_ERROR(LOG_R6, "R6 fixture failed at stage {} after {} frames", gStage, gFrames);
    }
    gFailed = failed;
    gDone = true;
    Report(gFrames, failed ? 1 : 0, 1, gCompute ? 1 : 0, 40, 405);
    rhi::Shutdown();
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
}
bool Accepted(rhi::RasterStatus status) noexcept
{
    const bool accepted = status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
    if (accepted == false)
    {
        LUDUS_LOG_ERROR(LOG_R6, "R6 creation rejected with status {}", static_cast<uint32>(status));
    }
    return accepted;
}
bool Checked(rhi::RasterStatus status) noexcept
{
    if (status != rhi::RasterStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_R6, "R6 operation at stage {} returned status {}", gStage, static_cast<uint32>(status));
        return false;
    }
    return true;
}
template <typename T, usize N>
std::span<const uint8> Bytes(const T (&values)[N]) noexcept
{
    return {reinterpret_cast<const uint8*>(values), sizeof(values)};
}
template <typename T>
bool Ready(T handle) noexcept
{
    const auto status = rhi::GetStatus(gDevice, handle);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        LUDUS_LOG_ERROR(LOG_R6, "R6 resource readiness failed with status {}", static_cast<uint32>(status));
        Stop(true);
    }
    return status == rhi::RasterStatus::Ready;
}
bool Create() noexcept
{
    const float32 points[20]{-.4F, -.75F, .25F, 0, 1, -.4F, .75F,  .25F, 0, 0,
                             .4F,  .75F,  .25F, 1, 0, .4F,  -.75F, .25F, 1, 1};
    const float32 offsets[4]{-.5F, 0, .5F, 0};
    const uint32 indices[6]{0, 1, 2, 0, 2, 3};
    const float32 tint[4]{1, 1, 1, 1};
    const uint8 image[16]{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const auto vertex = ludus::shaders::raster::Vertex();
    const auto fragment = ludus::shaders::raster::Fragment();
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(gDevice, info) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    gCompute = info.Enabled.Compute;
    if (Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Vertex, sizeof(points)}, Bytes(points), gVertices)) ==
            false ||
        Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Vertex, sizeof(offsets)}, Bytes(offsets), gInstances)) ==
            false ||
        Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Index32, sizeof(indices)}, Bytes(indices), gIndices)) ==
            false ||
        Accepted(rhi::CreateRasterShader(gDevice, fragment, gFragment)) == false)
    {
        return false;
    }
    if (gCompute)
    {
        const auto kernel = ludus::shaders::cull::Compute();
        if (Accepted(rhi::CreateRasterShader(gDevice, kernel, gComputeShader)) == false ||
            Accepted(rhi::CreateBindingLayout(gDevice, kernel.Bindings, gComputeLayout)) == false)
        {
            return false;
        }
    }
    for (usize i = 0; i < OBJECTS; ++i)
    {
        if (Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Uniform, sizeof(tint)}, Bytes(tint), gBuffers[i])) ==
                false ||
            Accepted(rhi::CreateTexture(gDevice, {2, 2}, {image, 8}, gTextures[i])) == false ||
            Accepted(rhi::CreateSampler(gDevice, {}, gSamplers[i])) == false ||
            Accepted(rhi::CreateRasterShader(gDevice, vertex, gShaders[i])) == false ||
            Accepted(rhi::CreateBindingLayout(gDevice, fragment.Bindings, gLayouts[i])) == false)
        {
            return false;
        }
    }
    gSlices[0] = {gVertices, 0, sizeof(points)};
    gSlices[1] = {gInstances, 0, sizeof(offsets)};
    return true;
}
bool IndependentReady() noexcept
{
    if (Ready(gVertices) == false || Ready(gInstances) == false || Ready(gIndices) == false ||
        Ready(gFragment) == false || (gCompute && (Ready(gComputeShader) == false || Ready(gComputeLayout) == false)))
    {
        return false;
    }
    for (usize i = 0; i < OBJECTS; ++i)
    {
        if (Ready(gBuffers[i]) == false || Ready(gTextures[i]) == false || Ready(gSamplers[i]) == false ||
            Ready(gShaders[i]) == false || Ready(gLayouts[i]) == false)
        {
            return false;
        }
    }
    return true;
}
bool Link() noexcept
{
    for (usize i = 0; i < OBJECTS; ++i)
    {
        const rhi::RasterBindingResource resources[]{{ .Buffer = gBuffers[i], .Size = 16 },
                                                     { .Binding = 1, .Texture = gViews[i] },
                                                     { .Binding = 2, .Sampler = gSamplers[i] }};
        if (Accepted(rhi::CreateBindingSet(gDevice, gLayouts[i], resources, gSets[i])) == false)
        {
            return false;
        }
        const rhi::RasterVertexStream streams[]{{20, false}, {8, true}};
        const rhi::RasterVertexAttribute attributes[]{{0, 0, 0, rhi::RasterVertexFormat::Float3},
                                                      {1, 0, 12, rhi::RasterVertexFormat::Float2},
                                                      {2, 1, 0, rhi::RasterVertexFormat::Float2}};
        if (Accepted(rhi::CreateRasterPipeline(gDevice,
                                               {gShaders[i], gFragment, gLayouts[i], streams, attributes, false},
                                               gPipelines[i])) == false)
        {
            return false;
        }
        if (gCompute &&
            Accepted(rhi::CreateComputePipeline(gDevice, {gComputeShader, gComputeLayout}, gKernels[i])) == false)
        {
            return false;
        }
    }
    for (usize i = 0; i < 1100; ++i)
    {
        const auto index = i % OBJECTS;
        gDraws[i] = {gPipelines[index], gSets[index], gSlices, gIndices, 0, 6, 4, 2};
    }
    return true;
}
bool AllReady() noexcept
{
    if (Ready(gVertices) == false || Ready(gInstances) == false || Ready(gIndices) == false ||
        Ready(gFragment) == false)
    {
        return false;
    }
    for (usize i = 0; i < OBJECTS; ++i)
    {
        if (Ready(gBuffers[i]) == false || Ready(gTextures[i]) == false || Ready(gViews[i]) == false ||
            Ready(gSamplers[i]) == false || Ready(gShaders[i]) == false || Ready(gLayouts[i]) == false ||
            Ready(gSets[i]) == false || Ready(gPipelines[i]) == false || (gCompute && Ready(gKernels[i]) == false))
        {
            return false;
        }
    }
    return true;
}
bool DrawBatch(usize count) noexcept
{
    rhi::CommandBatch batch;
    if (rhi::BeginCommands(gDevice, batch) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    for (usize i = 0; i < count; ++i)
    {
        if (rhi::RecordDraw(gDevice, batch, gDraws[i]) != rhi::RasterStatus::Ready)
        {
            return false;
        }
    }
    if (rhi::FinishCommands(gDevice, batch) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    rhi::SubmissionToken completion;
    const auto status = rhi::SubmitCommands(gDevice, gSurface, batch, completion);
    if (status == rhi::RasterStatus::NotReady)
    {
        gRetry = Checked(rhi::DiscardCommands(gDevice, batch));
        return false;
    }
    return Checked(status);
}
bool DrawGraph() noexcept
{
    gStage = 61;
    rhi::OrderedGraph graph;
    if (Checked(rhi::CreateOrderedGraph(gDevice, graph)) == false)
    {
        return false;
    }
    rhi::GraphUse uses[147];
    rhi::GraphVersion vertex, instance, indices;
    if (Checked(rhi::ImportGraphBuffer(gDevice, graph, gVertices, vertex)) == false ||
        Checked(rhi::ImportGraphBuffer(gDevice, graph, gInstances, instance)) == false ||
        Checked(rhi::ImportGraphBuffer(gDevice, graph, gIndices, indices)) == false)
    {
        return false;
    }
    uses[0] = {vertex, rhi::GraphAccessMode::Vertex, 0, 80};
    uses[1] = {instance, rhi::GraphAccessMode::Vertex, 0, 16};
    uses[2] = {indices, rhi::GraphAccessMode::Index, 0, 24};
    for (usize i = 0; i < OBJECTS; ++i)
    {
        rhi::GraphVersion uniform, texture;
        rhi::RasterTextureState state;
        if (Checked(rhi::ImportGraphBuffer(gDevice, graph, gBuffers[i], uniform)) == false ||
            Checked(rhi::GetTextureState(gDevice, gTextures[i], state)) == false ||
            Checked(rhi::ImportGraphTexture(gDevice,
                                            graph,
                                            gTextures[i],
                                            state,
                                            rhi::RasterTextureUse::SampledBoth,
                                            texture)) == false)
        {
            return false;
        }
        uses[3 + i * 2] = {uniform, rhi::GraphAccessMode::Uniform, 0, 16};
        uses[4 + i * 2] = {texture, rhi::GraphAccessMode::Sampled};
    }
    gStage = 63;
    for (uint32 i = 0; i < 40; ++i)
    {
        rhi::GraphPassDescription pass;
        pass.Name = "R6 capacity";
        pass.Line = i + 1;
        pass.Attachment.ColorLoad = i == 0 ? rhi::RasterLoad::Clear : rhi::RasterLoad::Load;
        pass.Uses = uses;
        pass.UseCount = 147;
        pass.Draws = gDraws + usize{i} * 10;
        pass.DrawCount = i == 39 ? 15 : 10;
        if (Checked(rhi::AddGraphPass(gDevice, graph, pass)) == false)
        {
            return false;
        }
    }
    gStage = 64;
    rhi::GraphReport report;
    rhi::GraphPassDiagnostic last;
    if (Checked(rhi::CompileOrderedGraph(gDevice, graph, report)) == false || report.PassCount != 40 ||
        report.ResourceCount != 147 || report.DrawCount != 405 || report.DiagnosticsTruncated == false ||
        Checked(rhi::GetGraphPassDiagnostic(gDevice, graph, 39, last)) == false || last.Line != 40)
    {
        return false;
    }
    gStage = 65;
    rhi::SubmissionToken completion;
    const auto status = rhi::ExecuteOrderedGraph(gDevice, gSurface, graph, completion);
    if (status == rhi::RasterStatus::NotReady)
    {
        gRetry = Checked(rhi::DiscardOrderedGraph(gDevice, graph));
        return false;
    }
    return Checked(status);
}
void Tick() noexcept
{
    if (Paused() != 0 || gDone)
    {
        return;
    }
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(gDevice, info) != rhi::DeviceStatus::Ready)
    {
        if (info.Startup.State != rhi::StartupState::Pending)
        {
            Stop(true);
        }
        return;
    }
    if (gCreated == false)
    {
        gStage = 1;
        if (Create() == false)
        {
            Stop(true);
            return;
        }
        gCreated = true;
    }
    if (gViewsCreated == false)
    {
        gStage = 2;
        if (IndependentReady() == false)
        {
            return;
        }
        gStage = 3;
        for (usize i = 0; i < OBJECTS; ++i)
        {
            if (Accepted(rhi::CreateTextureView(gDevice, gTextures[i], gViews[i])) == false)
            {
                Stop(true);
                return;
            }
        }
        gViewsCreated = true;
    }
    if (gLinked == false)
    {
        for (const auto view : gViews)
        {
            if (Ready(view) == false)
            {
                return;
            }
        }
        gStage = 4;
        if (Link() == false)
        {
            Stop(true);
            return;
        }
        gLinked = true;
    }
    gStage = 5;
    if (AllReady() == false)
    {
        return;
    }
    if (rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    gStage = 6;
    gRetry = false;
    const bool drawn = gFrames == 0 && Reference() == 0 ? DrawGraph() : DrawBatch(gFrames == 1 ? 1100 : 405);
    if (drawn == false)
    {
        if (gRetry)
        {
            return;
        }
        Stop(true);
        return;
    }
    ++gFrames;
    rhi::StorageUsage usage;
    if (rhi::GetStorageUsage(gDevice, usage) != rhi::RasterStatus::Ready ||
        usage.HostBytes > info.Profile.Storage.HostBytes || usage.ResourceBytes > info.Profile.Storage.ResourceBytes)
    {
        Stop(true);
        return;
    }
    Report(gFrames, 0, 0, gCompute ? 1 : 0, 40, 405);
    if (gFrames == 3)
    {
        Stop(false);
    }
}
} // namespace
int main()
{
    rhi::WindowInfo window;
    window.Width = 96;
    window.Height = 64;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo windowDescription;
    windowDescription.Width = 96;
    windowDescription.Height = 64;
    windowDescription.CanvasSelector = "#canvas";
    windowDescription.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(windowDescription, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.Compute = true;
    description.Storage.ResourcesPerKind = 128;
    description.Storage.DrawsPerFrame = 2048;
    description.Storage.DrawsPerPlan = 1536;
    description.Storage.ResourcesPerGraph = 160;
    description.Storage.PassesPerGraph = 64;
    description.Selection = Selection() == 1   ? rhi::BackendSelection::WebGPU
                            : Selection() == 2 ? rhi::BackendSelection::WebGL2
                                               : rhi::BackendSelection::Auto;
    const auto result = rhi::CreateDevice({}, window, description, gDevice, gSurface);
    if (result != rhi::DeviceStatus::Ready && result != rhi::DeviceStatus::Pending)
    {
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const auto started = time::NowTicks();
    while (gDone == false && time::NowTicks() - started < 30 * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (gDone == false)
    {
        Stop(true);
    }
#endif
    return gFailed ? 1 : 0;
}
