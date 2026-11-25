#include "r11compute.h"
#include "r11compute_slots.h"
#include "r11scalar.h"
#include "r11scalar_slots.h"
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/platform/base/window.h>

#include <cstring>
#include <span>

#include <emscripten.h>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
// Slots and the vertex input location that the LUSL compiler assigned to r11.lusl and
// r11compute.lusl.
namespace scalar_slots = ludus::shaders::r11scalar::slots;
namespace compute_slots = ludus::shaders::r11compute::slots;
constexpr uint32 WIDTH = 16;
constexpr uint32 HEIGHT = 4;
constexpr usize ALIGNMENT = 256;
constexpr usize UNIFORM_BYTES = ALIGNMENT * 4;
constexpr usize PIXEL_BYTES = usize{WIDTH} * HEIGHT * 4;
constexpr float32 COLORS[4][4]{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
core::UniquePtr<ludus::platform::Window> gWindow;
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::BufferHandle gUniform;
rhi::BufferHandle gVertices;
rhi::BufferHandle gIndices;
rhi::TextureHandle gTarget;
rhi::RasterShaderHandle gVertex;
rhi::RasterShaderHandle gFragment;
rhi::BindingLayoutHandle gLayout;
rhi::BindingSetHandle gSet;
rhi::RasterPipelineHandle gPipeline;
rhi::ReadbackTicket gReadback;
rhi::RasterVertexSlice gSlice;
rhi::BufferHandle gComputeBuffer;
rhi::RasterShaderHandle gComputeShader;
rhi::BindingLayoutHandle gComputeLayout;
rhi::BindingSetHandle gComputeSet;
rhi::ComputePipelineHandle gComputePipeline;
constexpr usize COMPUTE_BYTES = 512;
uint32 gStage = 0;
uint32 gFrames = 0;
// Application conformance reporting, outside the engine API.
// clang-format off
EM_JS(int32,GetSelection,(),{return new URL(location.href).searchParams.get('backend')==='webgl2'?2:1;});
EM_JS(void,Report,(uint32 stage,uint32 frames,int32 failed,int32 done),{const status=document.getElementById('status');status.dataset.state=failed?'failed':done?'passed':'loading';status.dataset.stage=stage;status.dataset.frames=frames;});
EM_JS(void,ReportFailure,(const char* operation,uint32 status),{console.error('R11 '+UTF8ToString(operation)+' status='+status);document.getElementById('status').dataset.operation=UTF8ToString(operation);document.getElementById('status').dataset.code=status;});
EM_JS(void,ReportCompute,(int32 hasRun),{document.getElementById('status').dataset.compute=hasRun?'passed':'unavailable';});
// clang-format on
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
void Finish(bool hasPassed) noexcept
{
    Report(gStage, gFrames, hasPassed == true ? 0 : 1, 1);
    rhi::Shutdown();
    emscripten_cancel_main_loop();
}
rhi::RasterDraw MakeDraw(uint32 color) noexcept
{
    rhi::RasterDraw draw
    {
        .Pipeline = gPipeline,
        .Bindings = gSet,
        .Vertices = {&gSlice, 1},
        .Indices = gIndices,
        .IndexCount = 6,
        .VertexCount = 4,
    };
    draw.BindingOffsets.Bytes[0] = color * static_cast<uint32>(ALIGNMENT);
    return draw;
}
bool Render(bool isGraph) noexcept
{
    if (rhi::SetFrameTarget(gDevice, gSurface, { .Width = WIDTH, .Height = HEIGHT, .Alpha = 1 }) !=
        rhi::DeviceStatus::Ready)
    {
        return false;
    }
    if (isGraph == true)
    {
        rhi::OrderedGraph graph;
        rhi::GraphVersion vertices;
        rhi::GraphVersion indices;
        rhi::GraphVersion uniform;
        rhi::GraphVersion target;
        rhi::RasterTextureState state;
        if (rhi::CreateOrderedGraph(gDevice, graph) != rhi::RasterStatus::Ready ||
            rhi::ImportGraphBuffer(gDevice, graph, gVertices, vertices) != rhi::RasterStatus::Ready ||
            rhi::ImportGraphBuffer(gDevice, graph, gIndices, indices) != rhi::RasterStatus::Ready ||
            rhi::ImportGraphBuffer(gDevice, graph, gUniform, uniform) != rhi::RasterStatus::Ready ||
            rhi::GetTextureState(gDevice, gTarget, state) != rhi::RasterStatus::Ready ||
            rhi::ImportGraphTexture(gDevice, graph, gTarget, state, rhi::RasterTextureUse::ColorAttachment, target) !=
                rhi::RasterStatus::Ready)
        {
            return false;
        }
        for (uint32 color = 0; color < 4; ++color)
        {
            rhi::GraphVersion output;
            if (rhi::NextGraphVersion(gDevice, graph, target, output) != rhi::RasterStatus::Ready)
            {
                return false;
            }
            const rhi::GraphUse uses[]{
                { .Resource = vertices, .Access = rhi::GraphAccessMode::Vertex, .Size = gSlice.Size },
                { .Resource = indices, .Access = rhi::GraphAccessMode::Index, .Size = 12 },
                {
                    .Resource = uniform,
                    .Access = rhi::GraphAccessMode::Uniform,
                    .Offset = color * ALIGNMENT,
                    .Size = 16,
                    .Visibility = rhi::RasterVisibility::Fragment,
                },
                {
                    .Resource = output,
                    .Access = color == 0 ? rhi::GraphAccessMode::ColorWrite : rhi::GraphAccessMode::ColorReadWrite,
                },
            };
            const rhi::RasterDraw draw = MakeDraw(color);
            const rhi::GraphPassDescription pass
            {
                .Name = "browser dynamic binding",
                .Attachment =
                {
                    .Color = gTarget,
                    .ColorLoad = color == 0 ? rhi::RasterLoad::Clear : rhi::RasterLoad::Load,
                    .Scissor = { .X = color * WIDTH / 4, .Width = WIDTH / 4, .Height = HEIGHT },
                    .UseScissor = true,
                },
                .Uses = uses,
                .UseCount = 4,
                .Draws = &draw,
                .DrawCount = 1,
                .SideEffect = true,
            };
            if (rhi::AddGraphPass(gDevice, graph, pass) != rhi::RasterStatus::Ready)
            {
                return false;
            }
            target = output;
        }
        rhi::GraphReport report;
        rhi::SubmissionToken submission;
        return rhi::CompileOrderedGraph(gDevice, graph, report) == rhi::RasterStatus::Ready &&
               rhi::ExecuteOrderedGraph(gDevice, gSurface, graph, submission) == rhi::RasterStatus::Ready;
    }
    const rhi::DeviceStatus acquired = rhi::BeginFrame(gDevice, gSurface);
    if (acquired != rhi::DeviceStatus::Ready)
    {
        ReportFailure("acquire", static_cast<uint32>(acquired));
        return false;
    }
    for (uint32 color = 0; color < 4; ++color)
    {
        const rhi::RasterPassDescription pass
        {
            .Color = gTarget,
            .ColorLoad = color == 0 ? rhi::RasterLoad::Clear : rhi::RasterLoad::Load,
            .Scissor = { .X = color * WIDTH / 4, .Width = WIDTH / 4, .Height = HEIGHT },
            .UseScissor = true,
        };
        const rhi::RasterStatus begun = rhi::BeginRasterPass(gDevice, pass);
        if (begun != rhi::RasterStatus::Ready)
        {
            ReportFailure("pass", static_cast<uint32>(begun));
            return false;
        }
        rhi::RasterDraw draw = MakeDraw(color);
        if (color == 0)
        {
            draw.BindingOffsets.Bytes[0] = 1;
            if (rhi::DrawIndexed(gDevice, draw) != rhi::RasterStatus::InvalidDescription)
            {
                return false;
            }
            draw.BindingOffsets.Bytes[0] = 0;
        }
        const rhi::RasterStatus drawn = rhi::DrawIndexed(gDevice, draw);
        if (drawn != rhi::RasterStatus::Ready)
        {
            ReportFailure("draw", static_cast<uint32>(drawn));
            return false;
        }
    }
    return rhi::EndFrame(gDevice) == rhi::DeviceStatus::Ready;
}
void Tick() noexcept
{
    rhi::DeviceInfo info;
    const rhi::DeviceStatus state = rhi::GetDeviceInfo(gDevice, info);
    if (state == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (state != rhi::DeviceStatus::Ready)
    {
        Finish(false);
        return;
    }
    (void)rhi::PollLifetime(gDevice);
    if (gStage == 0)
    {
        const float32 vertices[8]{-1, -1, 1, -1, -1, 1, 1, 1};
        const uint16 indices[6]{0, 1, 2, 2, 1, 3};
        uint8 uniform[UNIFORM_BYTES]{};
        for (usize color = 0; color < 4; ++color)
        {
            std::memcpy(uniform + color * ALIGNMENT, COLORS[color], sizeof(COLORS[color]));
        }
        if (IsAccepted(rhi::CreateBuffer(gDevice,
                                         { .Role = rhi::BufferRole::Vertex, .Size = sizeof(vertices) },
                                         {reinterpret_cast<const uint8*>(vertices), sizeof(vertices)},
                                         gVertices)) == false ||
            IsAccepted(rhi::CreateBuffer(gDevice,
                                         { .Role = rhi::BufferRole::Index16, .Size = sizeof(indices) },
                                         {reinterpret_cast<const uint8*>(indices), sizeof(indices)},
                                         gIndices)) == false ||
            IsAccepted(rhi::CreateBuffer(gDevice,
                                         { .Role = rhi::BufferRole::Uniform, .Size = sizeof(uniform) },
                                         uniform,
                                         gUniform)) == false ||
            IsAccepted(
                rhi::CreateTexture(gDevice, { .Width = WIDTH, .Height = HEIGHT, .Attachment = true }, {}, gTarget)) ==
                false ||
            IsAccepted(rhi::CreateRasterShader(gDevice, ludus::shaders::r11scalar::Vertex(), gVertex)) == false ||
            IsAccepted(rhi::CreateRasterShader(gDevice, ludus::shaders::r11scalar::Fragment(), gFragment)) == false)
        {
            Finish(false);
            return;
        }
        gSlice = { .Buffer = gVertices, .Size = sizeof(vertices) };
        ++gStage;
    }
    if (gStage == 1)
    {
        if (rhi::GetStatus(gDevice, gVertex) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gFragment) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gUniform) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gVertices) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gIndices) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gTarget) == rhi::RasterStatus::Pending)
        {
            return;
        }
        const rhi::RasterBinding binding
        {
            .Binding = scalar_slots::SETTINGS_BUFFER,
            .Kind = rhi::RasterBindingKind::UniformBuffer,
            .Visibility = rhi::RasterVisibility::Fragment,
            .MinSize = 16,
            .HasDynamicOffset = true,
        };
        if (IsAccepted(rhi::CreateBindingLayout(gDevice, {&binding, 1}, gLayout)) == false)
        {
            Finish(false);
            return;
        }
        ++gStage;
    }
    if (gStage == 2)
    {
        if (rhi::GetStatus(gDevice, gLayout) == rhi::RasterStatus::Pending)
        {
            return;
        }
        const rhi::RasterBindingResource resource
        {
            .Binding = scalar_slots::SETTINGS_BUFFER,
            .Buffer = gUniform,
            .Size = 16,
        };
        const rhi::RasterVertexStream stream{ .Stride = 8 };
        const rhi::RasterVertexAttribute attribute
        {
            .Location = ludus::shaders::r11scalar::locations::INPUT_POSITION_CLIP,
            .Format = rhi::RasterVertexFormat::Float2,
        };
        const rhi::RasterPipelineDescription description
        {
            .Vertex = gVertex,
            .Fragment = gFragment,
            .Layout = gLayout,
            .Streams = {&stream, 1},
            .Attributes = {&attribute, 1},
            .Target = rhi::RasterTarget::Rgba8Unorm,
        };
        if (IsAccepted(rhi::CreateBindingSet(gDevice, gLayout, {&resource, 1}, gSet)) == false ||
            IsAccepted(rhi::CreateRasterPipeline(gDevice, description, gPipeline)) == false)
        {
            Finish(false);
            return;
        }
        ++gStage;
    }
    if (gStage == 3 || gStage == 5)
    {
        if (rhi::GetStatus(gDevice, gPipeline) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gSet) == rhi::RasterStatus::Pending)
        {
            return;
        }
        const rhi::RasterStatus pipelineStatus = rhi::GetStatus(gDevice, gPipeline);
        const rhi::RasterStatus setStatus = rhi::GetStatus(gDevice, gSet);
        if (pipelineStatus != rhi::RasterStatus::Ready || setStatus != rhi::RasterStatus::Ready)
        {
            ReportFailure("pipeline", static_cast<uint32>(pipelineStatus));
            ReportFailure("set", static_cast<uint32>(setStatus));
            Finish(false);
            return;
        }
        if (Render(gStage == 5) == false || rhi::RequestTextureReadback(gDevice,
                                                                        gTarget,
                                                                        { .Width = WIDTH, .Height = HEIGHT },
                                                                        usize{WIDTH} * 4,
                                                                        gReadback) != rhi::RasterStatus::Pending)
        {
            Finish(false);
            return;
        }
        ++gFrames;
        ++gStage;
    }
    if (gStage == 4 || gStage == 6)
    {
        uint8 pixels[PIXEL_BYTES]{};
        const rhi::RasterStatus status = rhi::CopyReadback(gDevice, gReadback, pixels, sizeof(pixels));
        if (status == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (status != rhi::RasterStatus::Ready)
        {
            Finish(false);
            return;
        }
        for (usize pixel = 0; pixel < usize{WIDTH} * HEIGHT; ++pixel)
        {
            const usize color = (pixel % WIDTH) / (WIDTH / 4);
            for (usize channel = 0; channel < 4; ++channel)
            {
                if (pixels[pixel * 4 + channel] != static_cast<uint8>(COLORS[color][channel] * 255))
                {
                    Finish(false);
                    return;
                }
            }
        }
        (void)rhi::Release(gDevice, gReadback);
        if (gStage == 6 && info.Enabled.Compute == false)
        {
            ReportCompute(0);
            Finish(true);
            return;
        }
        ++gStage;
    }
    if (gStage == 7)
    {
        const uint8 bytes[COMPUTE_BYTES]{};
        const rhi::RasterBinding binding
        {
            .Binding = compute_slots::VALUES,
            .Kind = rhi::RasterBindingKind::StorageReadWrite,
            .Visibility = rhi::RasterVisibility::Compute,
            .MinSize = 4,
            .HasDynamicOffset = true,
        };
        if (IsAccepted(rhi::CreateBuffer(gDevice,
                                         { .Role = rhi::BufferRole::Storage, .Size = sizeof(bytes) },
                                         bytes,
                                         gComputeBuffer)) == false ||
            IsAccepted(rhi::CreateRasterShader(gDevice, ludus::shaders::r11compute::Compute(), gComputeShader)) ==
                false ||
            IsAccepted(rhi::CreateBindingLayout(gDevice, {&binding, 1}, gComputeLayout)) == false)
        {
            Finish(false);
            return;
        }
        ++gStage;
    }
    if (gStage == 8)
    {
        if (rhi::GetStatus(gDevice, gComputeBuffer) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gComputeShader) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gComputeLayout) == rhi::RasterStatus::Pending)
        {
            return;
        }
        const rhi::RasterBindingResource resource
        {
            .Binding = compute_slots::VALUES,
            .Buffer = gComputeBuffer,
            .Size = 16,
        };
        if (IsAccepted(rhi::CreateBindingSet(gDevice, gComputeLayout, {&resource, 1}, gComputeSet)) == false ||
            IsAccepted(rhi::CreateComputePipeline(gDevice,
                                                  { .Shader = gComputeShader, .Layout = gComputeLayout },
                                                  gComputePipeline)) == false)
        {
            Finish(false);
            return;
        }
        ++gStage;
    }
    if (gStage == 9)
    {
        if (rhi::GetStatus(gDevice, gComputePipeline) == rhi::RasterStatus::Pending ||
            rhi::GetStatus(gDevice, gComputeSet) == rhi::RasterStatus::Pending)
        {
            return;
        }
        rhi::OrderedGraph graph;
        rhi::GraphVersion input;
        rhi::GraphVersion output;
        if (rhi::CreateOrderedGraph(gDevice, graph) != rhi::RasterStatus::Ready ||
            rhi::ImportGraphBuffer(gDevice, graph, gComputeBuffer, input) != rhi::RasterStatus::Ready ||
            rhi::NextGraphVersion(gDevice, graph, input, output) != rhi::RasterStatus::Ready)
        {
            Finish(false);
            return;
        }
        rhi::ComputeDispatch dispatch{ .Pipeline = gComputePipeline, .Bindings = gComputeSet };
        dispatch.BindingOffsets.Bytes[0] = static_cast<uint32>(ALIGNMENT);
        const rhi::GraphUse use
        {
            .Resource = output,
            .Access = rhi::GraphAccessMode::StorageReadWrite,
            .Offset = ALIGNMENT,
            .Size = 16,
            .Visibility = rhi::RasterVisibility::Compute,
        };
        const rhi::GraphPassDescription pass
        {
            .Name = "dynamic browser storage",
            .Attachment = {},
            .Uses = &use,
            .UseCount = 1,
            .SideEffect = true,
            .Dispatch = &dispatch,
        };
        rhi::GraphReport report;
        rhi::SubmissionToken submission;
        if (rhi::AddGraphPass(gDevice, graph, pass) != rhi::RasterStatus::Ready ||
            rhi::CompileOrderedGraph(gDevice, graph, report) != rhi::RasterStatus::Ready ||
            rhi::ExecuteOrderedGraph(gDevice, gSurface, graph, submission) != rhi::RasterStatus::Ready ||
            rhi::RequestBufferReadback(gDevice, gComputeBuffer, 0, COMPUTE_BYTES, gReadback) !=
                rhi::RasterStatus::Pending)
        {
            Finish(false);
            return;
        }
        ++gFrames;
        ++gStage;
    }
    if (gStage == 10)
    {
        uint8 bytes[COMPUTE_BYTES]{};
        const rhi::RasterStatus copied = rhi::CopyReadback(gDevice, gReadback, bytes, sizeof(bytes));
        if (copied == rhi::RasterStatus::Pending)
        {
            return;
        }
        if (copied != rhi::RasterStatus::Ready)
        {
            Finish(false);
            return;
        }
        for (usize offset = 0; offset < COMPUTE_BYTES; offset += sizeof(uint32))
        {
            uint32 value = 0;
            std::memcpy(&value, bytes + offset, sizeof(value));
            if (value != (offset == ALIGNMENT ? 0x12345678U : 0U))
            {
                Finish(false);
                return;
            }
        }
        (void)rhi::Release(gDevice, gReadback);
        ReportCompute(1);
        Finish(true);
        return;
    }
    Report(gStage, gFrames, 0, 0);
}
} // namespace
int main()
{
    ludus::platform::WindowManager manager;
    const ludus::platform::WindowBase::CreateInfo window
    {
        .Name = {},
        .Width = WIDTH,
        .Height = HEIGHT,
        .CanvasSelector = "#canvas",
        .CaptureBrowserInput = false,
    };
    if (manager.Initialize({}) == false || manager.CreateWindow(window, gWindow) == false)
    {
        return 1;
    }
    const rhi::DeviceDescription description
    {
        .Selection = GetSelection() == 2 ? rhi::BackendSelection::WebGL2 : rhi::BackendSelection::WebGPU,
        .Limits = {},
        .Required = { .PortableRaster = true },
        .Preferred = {},
        .Storage = {},
        .RequiredProfile = {},
        .PreferredProfile = {},
    };
    const rhi::DeviceStatus created =
        rhi::CreateDevice({}, gWindow->GetNativeWindowInfo(), description, gDevice, gSurface);
    if (created != rhi::DeviceStatus::Pending && created != rhi::DeviceStatus::Ready)
    {
        return 1;
    }
    emscripten_set_main_loop(Tick, 0, 1);
    return 0;
}
