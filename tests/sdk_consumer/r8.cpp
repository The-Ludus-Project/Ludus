#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "raster.h"

#include <cstring>
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
constexpr logging::LogCategory LOG_R8{"R8-Fixture"};
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::BufferHandle gTint, gVertices, gInstances, gIndices;
rhi::TextureHandle gSource, gDestination, gTarget;
rhi::TextureViewHandle gView;
rhi::SamplerHandle gSampler;
rhi::BindingLayoutHandle gLayout;
rhi::BindingSetHandle gSet;
rhi::RasterShaderHandle gVertex, gFragment;
rhi::RasterPipelineHandle gPipeline, gOffscreenPipeline;
rhi::TransferTicket gWrites[2];
rhi::ReadbackTicket gReads[2];
rhi::RasterVertexSlice gSlices[2];
uint32 gStage = 0, gMip = 0, gFrames = 0;
bool gDone = false, gFailed = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// Application conformance reporting, outside the engine API.
// clang-format off
EM_JS(int32, Selection, (), { const b = new URL(location.href).searchParams.get('backend'); return b === 'webgpu' ? 1 : b === 'webgl2' ? 2 : 0; });
EM_JS(int32, Paused, (), { return globalThis.__qaPause ? 1 : 0; });
EM_JS(void, Report, (uint32 stage, uint32 frames, int32 failed, int32 done), {
    const n = document.getElementById('status'); n.dataset.state = failed ? 'failed' : done ? 'passed' : frames ? 'rendering' : 'loading';
    n.dataset.stage = stage; n.dataset.frames = frames; n.textContent = n.dataset.state + ' stage=' + stage;
});
// clang-format on
#else
int32 Selection() noexcept
{
    return 0;
}
int32 Paused() noexcept
{
    return 0;
}
void Report(uint32, uint32, int32, int32) noexcept {}
#endif
void Stop(bool failed) noexcept
{
    if (failed)
    {
        LUDUS_LOG_ERROR(LOG_R8, "R8 fixture failed at stage {} mip {}", gStage, gMip);
    }
    gDone = true;
    gFailed = failed;
    Report(gStage, gFrames, failed ? 1 : 0, 1);
    rhi::Shutdown();
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
}
bool Accepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
template <typename T>
bool Complete(T ticket) noexcept
{
    const auto status = rhi::GetStatus(gDevice, ticket);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        Stop(true);
    }
    return status == rhi::RasterStatus::Ready;
}
template <typename T, usize N>
std::span<const uint8> Bytes(const T (&values)[N]) noexcept
{
    return {reinterpret_cast<const uint8*>(values), sizeof(values)};
}
constexpr uint8 COLORS[4][4]{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
bool StartMip() noexcept
{
    const uint32 width = 8U >> gMip, height = 4U >> gMip;
    const auto rows = height == 0 ? 1U : height;
    const usize pitch = usize{width} * 4 + 8;
    uint8 pixels[160]{};
    for (uint32 y = 0; y < rows; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            const auto quadrant = (y >= rows / 2 ? 2U : 0U) + (x >= width / 2 ? 1U : 0U);
            std::memcpy(pixels + usize{y} * pitch + usize{x} * 4, COLORS[quadrant], 4);
        }
    }
    const auto result =
        rhi::RequestTextureUpdate(gDevice, gSource, {gMip, 0, 0, width, rows}, pixels, pitch * rows, pitch, gWrites[0]);
    std::memset(pixels, 77, sizeof(pixels)); // Caller storage can be reused immediately.
    return result == rhi::RasterStatus::Pending;
}
bool VerifyRead(rhi::ReadbackTicket ticket, bool uniform) noexcept
{
    uint8 actual[40]{};
    if (rhi::CopyReadback(gDevice, ticket, actual, sizeof(actual)) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    if (uniform)
    {
        float32 values[4]{};
        std::memcpy(values, actual, sizeof(values));
        for (const auto value : values)
        {
            if (value != 1)
            {
                return false;
            }
        }
        return true;
    }
    for (usize y = 0; y < 2; ++y)
    {
        for (usize x = 0; x < 4; ++x)
        {
            const uint8 yellow[4]{255, 255, 0, 255};
            const auto* expected = gStage == 12 && y == 0 && x < 2 ? yellow : COLORS[y * 2 + (x >= 2 ? 1 : 0)];
            if (std::memcmp(actual + y * 20 + x * 4, expected, 4) != 0)
            {
                return false;
            }
        }
        for (usize padding = 16; padding < 20; ++padding)
        {
            if (actual[y * 20 + padding] != 0)
            {
                return false;
            }
        }
    }
    return true;
}
bool CreateDraw() noexcept
{
    if (Accepted(rhi::CreateTextureView(gDevice, gDestination, gView)) == false ||
        Accepted(rhi::CreateSampler(gDevice, {}, gSampler)) == false)
    {
        return false;
    }
    auto vertex = ludus::shaders::raster::Vertex();
    auto fragment = ludus::shaders::raster::Fragment();
    return Accepted(rhi::CreateRasterShader(gDevice, vertex, gVertex)) &&
           Accepted(rhi::CreateRasterShader(gDevice, fragment, gFragment)) &&
           Accepted(rhi::CreateBindingLayout(gDevice, fragment.Bindings, gLayout));
}
void Tick() noexcept
{
    if (gDone || Paused() != 0)
    {
        return;
    }
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(gDevice, info) != rhi::DeviceStatus::Ready)
    {
        if (info.Startup.State == rhi::StartupState::Pending)
        {
            return;
        }
        Stop(true);
        return;
    }
    if (rhi::PollLifetime(gDevice) != rhi::RasterStatus::Ready)
    {
        Stop(true);
        return;
    }
    if (gStage == 0)
    {
        const uint8 zero[80]{};
        const uint16 indices[6]{0, 1, 2, 2, 1, 3};
        const float32 offsets[2]{};
        rhi::TextureDescription source;
        source.Width = 8;
        source.Height = 4;
        source.MipLevels = 3;
        source.DeferredInitialization = true;
        auto destination = source;
        destination.Width = 4;
        destination.Height = 2;
        destination.MipLevels = 1;
        if (Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Uniform, 16}, {zero, 16}, gTint)) == false ||
            Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Vertex, 80}, zero, gVertices)) == false ||
            Accepted(
                rhi::CreateBuffer(gDevice, {rhi::BufferRole::Vertex, sizeof(offsets)}, Bytes(offsets), gInstances)) ==
                false ||
            Accepted(
                rhi::CreateBuffer(gDevice, {rhi::BufferRole::Index16, sizeof(indices)}, Bytes(indices), gIndices)) ==
                false ||
            Accepted(rhi::CreateTexture(gDevice, source, {}, gSource)) == false ||
            Accepted(rhi::CreateTexture(gDevice, destination, {}, gDestination)) == false ||
            Accepted(rhi::CreateTexture(gDevice,
                                        {
                                            .Width = 4,
                                            .Height = 2,
                                            .Attachment = true,
                                        },
                                        {},
                                        gTarget)) == false)
        {
            Stop(true);
            return;
        }
        gStage = 1;
    }
    if (gStage == 1)
    {
        if (Complete(gTint) == false || Complete(gVertices) == false || Complete(gSource) == false ||
            Complete(gDestination) == false || Complete(gTarget) == false || Complete(gIndices) == false ||
            Complete(gInstances) == false)
        {
            return;
        }
        const float32 white[4]{1, 1, 1, 1};
        const float32 vertices[20]{-1, 1, 0, 0, 0, -1, -1, 0, 0, 1, 1, 1, 0, 1, 0, 1, -1, 0, 1, 1};
        if (rhi::RequestBufferUpdate(gDevice,
                                     gTint,
                                     0,
                                     reinterpret_cast<const uint8*>(white),
                                     sizeof(white),
                                     gWrites[0]) != rhi::RasterStatus::Pending ||
            rhi::RequestBufferUpdate(gDevice,
                                     gVertices,
                                     0,
                                     reinterpret_cast<const uint8*>(vertices),
                                     sizeof(vertices),
                                     gWrites[1]) != rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 2;
    }
    if (gStage == 2)
    {
        if (Complete(gWrites[0]) == false || Complete(gWrites[1]) == false)
        {
            return;
        }
        if (rhi::Release(gDevice, gWrites[0]) != rhi::RasterStatus::Ready ||
            rhi::Release(gDevice, gWrites[1]) != rhi::RasterStatus::Ready || StartMip() == false)
        {
            Stop(true);
            return;
        }
        gStage = 3;
    }
    if (gStage == 3)
    {
        if (Complete(gWrites[0]) == false)
        {
            return;
        }
        if (rhi::Release(gDevice, gWrites[0]) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        ++gMip;
        if (gMip < 3)
        {
            rhi::TextureViewHandle forbidden;
            if (rhi::CreateTextureView(gDevice, gSource, forbidden) != rhi::RasterStatus::InvalidState ||
                StartMip() == false)
            {
                Stop(true);
            }
            return;
        }
        rhi::TextureResidency residency;
        if (rhi::GetTextureResidency(gDevice, gSource, residency) != rhi::RasterStatus::Ready ||
            residency.InitializedMips != 7 || residency.InitializedBytes != 168 ||
            rhi::RequestTextureCopy(gDevice, gSource, {0, 2, 1, 4, 2}, gDestination, {0, 0, 0, 4, 2}, gWrites[0]) !=
                rhi::RasterStatus::Pending ||
            rhi::Destroy(gDevice, gSource) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gStage = 4;
    }
    if (gStage == 4)
    {
        if (Complete(gWrites[0]) == false)
        {
            return;
        }
        if (rhi::Release(gDevice, gWrites[0]) != rhi::RasterStatus::Ready ||
            rhi::RequestTextureReadback(gDevice, gDestination, {0, 0, 0, 4, 2}, 20, gReads[0]) !=
                rhi::RasterStatus::Pending ||
            rhi::RequestBufferReadback(gDevice, gTint, 0, 16, gReads[1]) != rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 5;
    }
    if (gStage == 5)
    {
        if (Complete(gReads[0]) == false || Complete(gReads[1]) == false)
        {
            return;
        }
        if (VerifyRead(gReads[0], false) == false || VerifyRead(gReads[1], true) == false ||
            rhi::Release(gDevice, gReads[0]) != rhi::RasterStatus::Ready ||
            rhi::Release(gDevice, gReads[1]) != rhi::RasterStatus::Ready || CreateDraw() == false)
        {
            Stop(true);
            return;
        }
        gStage = 6;
    }
    if (gStage == 6)
    {
        if (Complete(gView) == false || Complete(gSampler) == false || Complete(gVertex) == false ||
            Complete(gFragment) == false || Complete(gLayout) == false)
        {
            return;
        }
        const rhi::RasterBindingResource resources[3]{{ .Buffer = gTint, .Size = 16 },
                                                      { .Binding = 1, .Texture = gView },
                                                      { .Binding = 2, .Sampler = gSampler }};
        const rhi::RasterVertexStream streams[2]{{20, false}, {8, true}};
        const rhi::RasterVertexAttribute attributes[3]{{0, 0, 0, rhi::RasterVertexFormat::Float3},
                                                       {1, 0, 12, rhi::RasterVertexFormat::Float2},
                                                       {2, 1, 0, rhi::RasterVertexFormat::Float2}};
        rhi::RasterPipelineDescription pipeline;
        pipeline.Vertex = gVertex;
        pipeline.Fragment = gFragment;
        pipeline.Layout = gLayout;
        pipeline.Streams = streams;
        pipeline.Attributes = attributes;
        pipeline.Depth = false;
        if (Accepted(rhi::CreateBindingSet(gDevice, gLayout, resources, gSet)) == false ||
            Accepted(rhi::CreateRasterPipeline(gDevice, pipeline, gPipeline)) == false)
        {
            Stop(true);
            return;
        }
        pipeline.Target = rhi::RasterTarget::Rgba8Unorm;
        if (Accepted(rhi::CreateRasterPipeline(gDevice, pipeline, gOffscreenPipeline)) == false)
        {
            Stop(true);
            return;
        }
        gSlices[0] = {gVertices, 0, 80};
        gSlices[1] = {gInstances, 0, 8};
        gStage = 7;
    }
    if ((gStage == 7 || gStage == 9 || gStage == 11) &&
        rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready)
    {
        Stop(true);
        return;
    }
    if (gStage == 7)
    {
        if (Complete(gSet) == false || Complete(gPipeline) == false || Complete(gOffscreenPipeline) == false)
        {
            return;
        }
        const auto begin = rhi::BeginFrame(gDevice, gSurface);
        if (begin == rhi::DeviceStatus::Skipped)
        {
            return;
        }
        const auto pass = rhi::BeginRasterPass(gDevice, { .Color = gTarget });
        const auto draw = rhi::DrawIndexed(gDevice, {gOffscreenPipeline, gSet, gSlices, gIndices, 0, 6, 4, 1});
        const auto end = rhi::EndFrame(gDevice);
        const auto read = rhi::RequestTextureReadback(gDevice, gTarget, {0, 0, 0, 4, 2}, 20, gReads[0]);
        if (begin != rhi::DeviceStatus::Ready || pass != rhi::RasterStatus::Ready || draw != rhi::RasterStatus::Ready ||
            end != rhi::DeviceStatus::Ready || read != rhi::RasterStatus::Pending)
        {
            LUDUS_LOG_ERROR(LOG_R8,
                            "R8 render/read statuses {}, {}, {}, {}, {}",
                            static_cast<uint32>(begin),
                            static_cast<uint32>(pass),
                            static_cast<uint32>(draw),
                            static_cast<uint32>(end),
                            static_cast<uint32>(read));
            Stop(true);
            return;
        }
        gStage = 8;
    }
    if (gStage == 8)
    {
        if (Complete(gReads[0]) == false)
        {
            return;
        }
        if (VerifyRead(gReads[0], false) == false || rhi::Release(gDevice, gReads[0]) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gStage = 10;
    }
    if (gStage == 10)
    {
        const uint8 yellow[8]{255, 255, 0, 255, 255, 255, 0, 255};
        const auto update = rhi::RequestTextureUpdate(gDevice, gTarget, {0, 0, 0, 2, 1}, yellow, 8, 8, gWrites[0]);
        if (update == rhi::RasterStatus::InvalidState)
        {
            return;
        }
        if (update != rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 11;
    }
    if (gStage == 11)
    {
        if (Complete(gWrites[0]) == false)
        {
            return;
        }
        if (rhi::Release(gDevice, gWrites[0]) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        // Load without a color write must preserve both updated and untouched texels.
        if (rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready ||
            rhi::BeginFrame(gDevice, gSurface) != rhi::DeviceStatus::Ready ||
            rhi::BeginRasterPass(gDevice, { .Color = gTarget, .ColorLoad = rhi::RasterLoad::Load }) !=
                rhi::RasterStatus::Ready ||
            rhi::EndFrame(gDevice) != rhi::DeviceStatus::Ready ||
            rhi::RequestTextureReadback(gDevice, gTarget, {0, 0, 0, 4, 2}, 20, gReads[0]) != rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 12;
    }
    if (gStage == 12)
    {
        if (Complete(gReads[0]) == false)
        {
            return;
        }
        if (VerifyRead(gReads[0], false) == false || rhi::Release(gDevice, gReads[0]) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        gStage = 9;
    }
    if (gStage == 9)
    {
        const auto begin = rhi::BeginFrame(gDevice, gSurface);
        if (begin == rhi::DeviceStatus::Skipped)
        {
            return;
        }
        if (begin != rhi::DeviceStatus::Ready ||
            rhi::DrawIndexed(gDevice, {gPipeline, gSet, gSlices, gIndices, 0, 6, 4, 1}) != rhi::RasterStatus::Ready ||
            rhi::EndFrame(gDevice) != rhi::DeviceStatus::Ready)
        {
            Stop(true);
            return;
        }
        ++gFrames;
        Report(gStage, gFrames, 0, 0);
        if (gFrames == 3)
        {
            Stop(false);
        }
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
    ludus::platform::WindowBase::CreateInfo description;
    description.Width = 96;
    description.Height = 64;
    description.CanvasSelector = "#canvas";
    description.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(description, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription device;
    device.Required.PortableRaster = true;
    device.RequiredProfile.FormatCount = 1;
    device.RequiredProfile.Formats[0].Uses.CopySource = true;
    device.RequiredProfile.Formats[0].Uses.CopyDestination = true;
    device.Storage.TransferSlices = 2;
    device.Storage.TransferBytes = 4096;
    device.Selection = Selection() == 1   ? rhi::BackendSelection::WebGPU
                       : Selection() == 2 ? rhi::BackendSelection::WebGL2
                                          : rhi::BackendSelection::Auto;
    const auto created = rhi::CreateDevice({}, window, device, gDevice, gSurface);
    if (created != rhi::DeviceStatus::Ready && created != rhi::DeviceStatus::Pending)
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
