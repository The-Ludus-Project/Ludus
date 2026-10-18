#include "r10depth.h"
#include "r10mask.h"
#include "r10sample.h"
#include "r10stencil.h"
#include "r10write.h"
#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/graph.h>
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
constexpr logging::LogCategory LOG_R10{"R10-Fixture"};
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::DeviceProfile gProfile;
rhi::TextureHandle gTextures[9];
rhi::TextureViewHandle gAttachments[8], gSampled[7];
rhi::BufferHandle gVertices, gIndices;
rhi::BindingLayoutHandle gLayouts[4];
rhi::BindingSetHandle gEmpty;
rhi::TextureViewHandle gStencilView;
rhi::BindingSetHandle gSets[8];
rhi::RasterShaderHandle gVertex, gFragments[5];
rhi::RasterPipelineHandle gPipelines[7];
rhi::SamplerHandle gSampler;
rhi::ReadbackTicket gRead;
rhi::OrderedGraph gGraph;
uint32 gStage = 0, gCase = 0, gFrames = 0;
bool gDone = false, gFailed = false, gMsaa = false, gPartial = false, gDepth = false, gStencilSample = false,
     gCube = false;
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// Application conformance reporting, outside the engine API.
// clang-format off
EM_JS(int32,Selection,(),{const b=new URL(location.href).searchParams.get('backend');return b==='webgpu'?1:b==='webgl2'?2:0;});
EM_JS(void,Report,(uint32 stage,uint32 frames,int32 failed,int32 done),{const n=document.getElementById('status');n.dataset.state=failed?'failed':done?'passed':frames?'rendering':'loading';n.dataset.stage=stage;n.dataset.frames=frames;n.textContent=n.dataset.state+' stage='+stage;});
// clang-format on
#else
int32 Selection() noexcept
{
    return 0;
}
void Report(uint32, uint32, int32, int32) noexcept {}
#endif
void Stop(bool failed) noexcept
{
    gFailed = failed;
    gDone = true;
    if (failed)
    {
        LUDUS_LOG_ERROR(LOG_R10, "R10 failed at stage {} case {}", gStage, gCase);
    }
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
template <class T>
bool Ready(T handle) noexcept
{
    const auto status = rhi::GetStatus(gDevice, handle);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        Stop(true);
    }
    return status == rhi::RasterStatus::Ready;
}
template <class T, usize N>
std::span<const uint8> Bytes(const T (&array)[N]) noexcept
{
    return {reinterpret_cast<const uint8*>(array), sizeof(array)};
}
bool Resources() noexcept
{
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(gDevice, info) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    gProfile = info.Profile;
    const auto& caps = gProfile.Formats[0];
    gMsaa = (caps.SampleCounts & 4) != 0;
    gPartial = caps.MaxAttachmentMipLevels >= 2 && caps.MaxAttachmentLayers >= 2;
    gCube = caps.MaxAttachmentCubeDimension >= 8 && caps.MaxAttachmentMipLevels >= 2;
    gDepth = caps.DepthSampled;
    gStencilSample = caps.StencilSampled;
    if (gProfile.Interface.MaxColorAttachments < 2 || caps.StencilAttachment == false)
    {
        return false;
    }
    for (usize i = 0; i < 9; ++i)
    {
        rhi::TextureDescription description{ .Width = 8, .Height = 8, .Attachment = true };
        if (i < 2 || (i >= 2 && i < 4))
        {
            description.Stencil = true;
        }
        if (i >= 2 && i < 4)
        {
            description.SampleCount = gMsaa ? 4U : 1U;
        }
        if (i == 6 && gPartial)
        {
            description.MipLevels = 2;
            description.Dimension = rhi::TextureDimension::D2Array;
            description.DepthOrLayers = 2;
        }
        if (i == 8 && gCube)
        {
            description.MipLevels = 2;
            description.Dimension = rhi::TextureDimension::Cube;
            description.DepthOrLayers = 6;
        }
        if (Accepted(rhi::CreateTexture(gDevice, description, {}, gTextures[i])) == false)
        {
            return false;
        }
    }
    const float32 vertices[16]{-1, 1, 0, 0, -1, -1, 0, 1, 1, 1, 1, 0, 1, -1, 1, 1};
    const uint16 indices[6]{0, 1, 2, 2, 1, 3};
    return Accepted(
               rhi::CreateBuffer(gDevice, {rhi::BufferRole::Vertex, sizeof(vertices)}, Bytes(vertices), gVertices)) &&
           Accepted(rhi::CreateBuffer(gDevice, {rhi::BufferRole::Index16, sizeof(indices)}, Bytes(indices), gIndices));
}
bool ViewsAndShaders() noexcept
{
    for (usize i = 0; i < 8; ++i)
    {
        rhi::TextureViewDescription description{ .MipCount = 1, .LayerCount = 1, .Attachment = true };
        if (i == 6 && gPartial)
        {
            description.BaseMip = 1;
            description.BaseLayer = 1;
        }
        if (i == 7 && gCube)
        {
            description.BaseMip = 1;
            description.BaseLayer = 4;
        }
        if (Accepted(rhi::CreateTextureView(gDevice, gTextures[i == 7 ? 8 : i], description, gAttachments[i])) == false)
        {
            return false;
        }
    }
    const usize textures[5]{0, 1, 4, 5, 6};
    for (usize i = 0; i < 5; ++i)
    {
        rhi::TextureViewDescription view{ .MipCount = 1, .LayerCount = 1 };
        if (i == 4 && gPartial)
        {
            view.BaseMip = 1;
            view.BaseLayer = 1;
        }
        if (Accepted(rhi::CreateTextureView(gDevice, gTextures[textures[i]], view, gSampled[i])) == false)
        {
            return false;
        }
    }
    rhi::TextureViewDescription cubeView{ .MipCount = 1, .LayerCount = 1 };
    if (gCube)
    {
        cubeView.BaseMip = 1;
        cubeView.BaseLayer = 4;
    }
    if (Accepted(rhi::CreateTextureView(gDevice, gTextures[8], cubeView, gSampled[6])) == false)
    {
        return false;
    }
    if (gDepth &&
        Accepted(rhi::CreateTextureView(gDevice, gTextures[0], { .Aspect = rhi::TextureAspect::Depth }, gSampled[5])) ==
            false)
    {
        return false;
    }
    auto& stencil = gStencilView;
    if (gStencilSample &&
        Accepted(rhi::CreateTextureView(gDevice, gTextures[0], { .Aspect = rhi::TextureAspect::Stencil }, stencil)) ==
            false)
    {
        return false;
    }
    if (Accepted(rhi::CreateSampler(gDevice, {}, gSampler)) == false ||
        Accepted(rhi::CreateRasterShader(gDevice, ludus::shaders::r10write::Vertex(), gVertex)) == false)
    {
        return false;
    }
    const rhi::RasterShaderDescription fragments[]{ludus::shaders::r10write::Fragment(),
                                                   ludus::shaders::r10mask::Fragment(),
                                                   ludus::shaders::r10sample::Fragment(),
                                                   ludus::shaders::r10depth::Fragment(),
                                                   ludus::shaders::r10stencil::Fragment()};
    for (usize i = 0; i < 5; ++i)
    {
        if ((i == 3 && gDepth == false) || (i == 4 && gStencilSample == false))
        {
            continue;
        }
        if (Accepted(rhi::CreateRasterShader(gDevice, fragments[i], gFragments[i])) == false)
        {
            return false;
        }
    }
    const rhi::RasterBinding color[2]{{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                      {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
    const rhi::RasterBinding depth{2, rhi::RasterBindingKind::TextureDepth2D, rhi::RasterVisibility::Fragment, 0};
    const rhi::RasterBinding stencilBinding{3,
                                            rhi::RasterBindingKind::TextureStencil2D,
                                            rhi::RasterVisibility::Fragment,
                                            0};
    return Accepted(rhi::CreateBindingLayout(gDevice, {}, gLayouts[0])) &&
           Accepted(rhi::CreateBindingLayout(gDevice, color, gLayouts[1])) &&
           (gDepth == false || Accepted(rhi::CreateBindingLayout(gDevice, {&depth, 1}, gLayouts[2]))) &&
           (gStencilSample == false || Accepted(rhi::CreateBindingLayout(gDevice, {&stencilBinding, 1}, gLayouts[3])));
}
bool PipelinesAndSets() noexcept
{
    if (gStencilSample)
    {
        const rhi::RasterBindingResource resource{ .Binding = 3, .Texture = gStencilView };
        if (Accepted(rhi::CreateBindingSet(gDevice, gLayouts[3], {&resource, 1}, gSets[6])) == false)
        {
            return false;
        }
        if (rhi::Destroy(gDevice, gStencilView) != rhi::RasterStatus::Ready)
        {
            return false;
        }
    }
    // Empty write bindings use an independent snapshot; sampled snapshots retain their aspect views.
    rhi::BindingSetHandle empty;
    if (Accepted(rhi::CreateBindingSet(gDevice, gLayouts[0], {}, empty)) == false)
    {
        return false;
    }
    for (usize i = 0; i < 5; ++i)
    {
        const rhi::RasterBindingResource resources[2]{
        {
            .Texture = gSampled[i],
        },
                                                      {
                                                          .Binding = 1,
                                                          .Sampler = gSampler,
                                                      }};
        if (Accepted(rhi::CreateBindingSet(gDevice, gLayouts[1], resources, gSets[i])) == false)
        {
            return false;
        }
    }
    const rhi::RasterBindingResource cubeResources[2]{
    {
        .Texture = gSampled[6],
    },
                                                      {
                                                          .Binding = 1,
                                                          .Sampler = gSampler,
                                                      }};
    if (Accepted(rhi::CreateBindingSet(gDevice, gLayouts[1], cubeResources, gSets[7])) == false)
    {
        return false;
    }
    if (gDepth)
    {
        const rhi::RasterBindingResource resource{ .Binding = 2, .Texture = gSampled[5] };
        if (Accepted(rhi::CreateBindingSet(gDevice, gLayouts[2], {&resource, 1}, gSets[5])) == false)
        {
            return false;
        }
    }
    const rhi::RasterVertexStream stream{16, false};
    const rhi::RasterVertexAttribute attributes[2]{{0, 0, 0, rhi::RasterVertexFormat::Float2},
                                                   {1, 0, 8, rhi::RasterVertexFormat::Float2}};
    const rhi::RasterFormat colors[2]{rhi::RasterFormat::Rgba8Unorm, rhi::RasterFormat::Rgba8Unorm};
    for (usize i = 0; i < 7; ++i)
    {
        if ((i == 5 && gDepth == false) || (i == 6 && gStencilSample == false))
        {
            continue;
        }
        rhi::RasterPipelineDescription pipeline;
        pipeline.Vertex = gVertex;
        pipeline.Fragment = gFragments[i < 2 ? 0 : i < 4 ? 1 : i == 4 ? 2 : i == 5 ? 3 : 4];
        pipeline.Layout = gLayouts[i < 4 ? 0 : i == 4 ? 1 : i == 5 ? 2 : 3];
        pipeline.Streams = {&stream, 1};
        pipeline.Attributes = attributes;
        pipeline.Target = rhi::RasterTarget::Rgba8Unorm;
        if (i < 4)
        {
            pipeline.ColorFormats = colors;
            pipeline.Depth = true;
            pipeline.Stencil = true;
            pipeline.StencilReference = i == 3 ? 8U : 7U;
            pipeline.StencilFront.Pass =
                i < 2 ? rhi::RasterStencilOperation::Replace : rhi::RasterStencilOperation::Keep;
            pipeline.StencilBack.Pass = rhi::RasterStencilOperation::Keep;
            pipeline.StencilFront.Compare = pipeline.StencilBack.Compare =
                i < 2 ? rhi::RasterStencilCompare::Always : rhi::RasterStencilCompare::Equal;
            if (i == 1)
            {
                pipeline.SampleCount = gMsaa ? 4U : 1U;
            }
            if (i >= 2)
            {
                pipeline.DepthWrite = false;
                pipeline.DepthCompare = rhi::RasterDepthCompare::LessEqual;
                pipeline.StencilWriteMask = 0;
            }
        }
        if (Accepted(rhi::CreateRasterPipeline(gDevice, pipeline, gPipelines[i])) == false)
        {
            return false;
        }
    }
    // Preserve the empty binding snapshot in a distinct global slot.
    gEmpty = empty;
    return true;
}
bool DrawCase(bool graph) noexcept;
bool Verify() noexcept
{
    uint8 pixels[256]{};
    if (rhi::CopyReadback(gDevice, gRead, pixels, sizeof(pixels)) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    for (usize i = 0; i < sizeof(pixels); ++i)
    {
        const usize c = i % 4, x = (i / 4) % 8, y = i / 32;
        uint8 expected = 255;
        const bool second = gCase == 2 || gCase == 3 || gCase == 5;
        if (c != 3)
        {
            if (gCase == 6 || gCase == 7 || gCase == 14 || gCase == 15)
            {
                expected = c == 0 ? 0 : 255;
            }
            else if ((gCase == 8 || gCase == 9) && gDepth)
            {
                expected = 64;
            }
            else if ((gCase == 10 || gCase == 11) && gStencilSample)
            {
                expected = 7;
            }
            else if (gCase == 12)
            {
                expected = c == 2 ? 0 : 255;
            }
            else if (gCase == 13)
            {
                expected = 0;
            }
            else
            {
                expected = second ? (c == 0   ? 0
                                     : c == 1 ? (y >= 4 ? 255 : 0)
                                              : (x >= 4 ? 255 : 0))
                                  : (c == 0   ? (x >= 4 ? 255 : 0)
                                     : c == 1 ? 0
                                              : (y >= 4 ? 255 : 0));
            }
        }
        const int32 difference = static_cast<int32>(pixels[i]) - expected;
        if (difference < -1 || difference > 1)
        {
            LUDUS_LOG_ERROR(LOG_R10, "R10 byte {} actual {} expected {}", i, pixels[i], expected);
            return false;
        }
    }
    return true;
}
bool DrawCase(bool graph) noexcept
{
    const bool cube = gCase == 14 || gCase == 15;
    const bool partial = gCase == 6 || gCase == 7 || cube;
    const bool msaa = (gCase == 4 || gCase == 5) && gMsaa;
    const bool masked = gCase == 12 || gCase == 13;
    rhi::RasterColorAttachment colors[2]{
    {
        .View = gAttachments[msaa ? 2 : 0],
    },
                                         {
                                             .View = gAttachments[msaa ? 3 : 1],
                                         }};
    if (msaa)
    {
        colors[0].Resolve = gAttachments[4];
        colors[1].Resolve = gAttachments[5];
        colors[0].Store = colors[1].Store = rhi::RasterStore::Discard;
    }
    if (partial)
    {
        colors[0].View = gAttachments[cube ? 7 : 6];
        colors[0].Clear[0] = 0;
        colors[0].Clear[1] = colors[0].Clear[2] = colors[0].Clear[3] = 1;
    }
    rhi::RasterPassDescription pass;
    pass.Colors = colors;
    pass.ColorCount = partial ? 1U : 2U;
    pass.DepthStore = rhi::RasterStore::Store;
    pass.StencilStore = rhi::RasterStore::Store;
    if (masked)
    {
        pass.DepthLoad = rhi::RasterLoad::Load;
        pass.DepthReadOnly = true;
        pass.StencilLoad = rhi::RasterLoad::Load;
        pass.StencilReadOnly = true;
    }
    const rhi::RasterVertexSlice slice{gVertices, 0, 64};
    const rhi::RasterDraw write{gPipelines[masked ? (gCase == 12 ? 2 : 3)
                                           : msaa ? 1
                                                  : 0],
                                gEmpty,
                                {&slice, 1},
                                gIndices,
                                0,
                                6,
                                4,
                                1};
    const usize set = partial                                          ? (cube ? 7 : 4)
                      : (gCase == 8 || gCase == 9) && gDepth           ? 5
                      : (gCase == 10 || gCase == 11) && gStencilSample ? 6
                      : gCase == 2 || gCase == 3 || gCase == 5         ? (msaa ? 3 : 1)
                                                                       : (msaa ? 2 : 0);
    const rhi::RasterDraw sample{gPipelines[set == 5   ? 5
                                            : set == 6 ? 6
                                                       : 4],
                                 gSets[set],
                                 {&slice, 1},
                                 gIndices,
                                 0,
                                 6,
                                 4,
                                 1};
    if (graph == false)
    {
        return rhi::BeginFrame(gDevice, gSurface) == rhi::DeviceStatus::Ready &&
               rhi::BeginRasterPass(gDevice, pass) == rhi::RasterStatus::Ready &&
               (partial || rhi::DrawIndexed(gDevice, write) == rhi::RasterStatus::Ready) &&
               rhi::BeginRasterPass(gDevice, { .Color = gTextures[7] }) == rhi::RasterStatus::Ready &&
               rhi::DrawIndexed(gDevice, sample) == rhi::RasterStatus::Ready &&
               rhi::EndFrame(gDevice) == rhi::DeviceStatus::Ready;
    }
    if (rhi::CreateOrderedGraph(gDevice, gGraph) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    rhi::GraphVersion imports[11], outputs[9];
    if (rhi::ImportGraphBuffer(gDevice, gGraph, gVertices, imports[0]) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphBuffer(gDevice, gGraph, gIndices, imports[1]) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    for (usize i = 0; i < 9; ++i)
    {
        rhi::RasterTextureState state;
        if (rhi::GetTextureState(gDevice, gTextures[i], state) != rhi::RasterStatus::Ready ||
            rhi::ImportGraphTexture(gDevice,
                                    gGraph,
                                    gTextures[i],
                                    state,
                                    rhi::RasterTextureUse::SampledBoth,
                                    imports[i + 2]) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        outputs[i] = imports[i + 2];
    }
    rhi::GraphUse uses[6]{{imports[0], rhi::GraphAccessMode::Vertex, 0, 64},
                          {imports[1], rhi::GraphAccessMode::Index, 0, 12}};
    usize count = 2;
    const usize targets[4]{partial ? (cube ? 8U : 6U) : msaa ? 2U : 0U, msaa ? 3U : 1U, 4U, 5U};
    const usize targetCount = partial ? 1 : msaa ? 4 : 2;
    for (usize t = 0; t < targetCount; ++t)
    {
        const auto i = targets[t];
        rhi::GraphVersion written;
        if (rhi::NextGraphVersion(gDevice, gGraph, imports[i + 2], written) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        outputs[i] = written;
        uses[count++] = {outputs[i], rhi::GraphAccessMode::ColorWrite};
    }
    rhi::GraphPassDescription producer;
    producer.Name = "R10 attachments";
    producer.Attachment = pass;
    producer.Uses = uses;
    producer.UseCount = count;
    producer.Draws = partial ? nullptr : &write;
    producer.DrawCount = partial ? 0 : 1;
    const auto authored = rhi::AddGraphPass(gDevice, gGraph, producer);
    if (authored != rhi::RasterStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_R10, "Producer status {}", static_cast<uint32>(authored));
        return false;
    }
    const usize sampledTexture = set == 5 || set == 6 ? 0
                                 : set == 7           ? 8
                                 : set == 4           ? 6
                                 : set == 3           ? 5
                                 : set == 2           ? 4
                                                      : set;
    rhi::GraphVersion composite;
    if (rhi::NextGraphVersion(gDevice, gGraph, imports[9], composite) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    outputs[7] = composite;
    rhi::GraphUse consumerUses[4]{{imports[0], rhi::GraphAccessMode::Vertex, 0, 64},
                                  {imports[1], rhi::GraphAccessMode::Index, 0, 12},
                                  {outputs[sampledTexture], rhi::GraphAccessMode::Sampled},
                                  {outputs[7], rhi::GraphAccessMode::ColorWrite}};
    rhi::GraphPassDescription consumer;
    consumer.Name = "R10 sampled result";
    consumer.Attachment.Color = gTextures[7];
    consumer.Uses = consumerUses;
    consumer.UseCount = 4;
    consumer.Draws = &sample;
    consumer.DrawCount = 1;
    rhi::GraphReport report;
    const auto authoredConsumer = rhi::AddGraphPass(gDevice, gGraph, consumer);
    const auto rooted = rhi::AddGraphRoot(gDevice, gGraph, outputs[7], rhi::GraphRoot::Readback);
    const auto compiled = rhi::CompileOrderedGraph(gDevice, gGraph, report);
    if (authoredConsumer != rhi::RasterStatus::Ready || rooted != rhi::RasterStatus::Ready ||
        compiled != rhi::RasterStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_R10,
                        "Graph consumer {} root {} compile {} pass {} resource {}",
                        static_cast<uint32>(authoredConsumer),
                        static_cast<uint32>(rooted),
                        static_cast<uint32>(compiled),
                        report.ErrorPass,
                        report.ErrorResource);
        return false;
    }
    if (gCase == 16)
    {
        for (usize i = 0; i < 2; ++i)
        {
            if (rhi::Destroy(gDevice, gAttachments[i]) != rhi::RasterStatus::Ready ||
                rhi::Destroy(gDevice, gTextures[i]) != rhi::RasterStatus::Ready)
            {
                return false;
            }
        }
    }
    rhi::SubmissionToken token;
    const auto executed = rhi::ExecuteOrderedGraph(gDevice, gSurface, gGraph, token);
    if (executed != rhi::RasterStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_R10, "Graph execute {}", static_cast<uint32>(executed));
    }
    return executed == rhi::RasterStatus::Ready;
}
void Tick() noexcept
{
    if (gDone)
    {
        return;
    }
    rhi::DeviceInfo info;
    const auto device = rhi::GetDeviceInfo(gDevice, info);
    if (device != rhi::DeviceStatus::Ready)
    {
        if (device != rhi::DeviceStatus::Pending)
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
        if (Resources() == false)
        {
            Stop(true);
            return;
        }
        gStage = 1;
    }
    if (gStage == 1)
    {
        for (auto texture : gTextures)
        {
            if (Ready(texture) == false)
            {
                return;
            }
        }
        if (Ready(gVertices) == false || Ready(gIndices) == false)
        {
            return;
        }
        if (ViewsAndShaders() == false)
        {
            Stop(true);
            return;
        }
        gStage = 2;
    }
    if (gStage == 2)
    {
        for (auto view : gAttachments)
        {
            if (Ready(view) == false)
            {
                return;
            }
        }
        for (usize i = 0; i < 6; ++i)
        {
            if ((i < 5 || gDepth) && Ready(gSampled[i]) == false)
            {
                return;
            }
        }
        for (usize i = 0; i < 5; ++i)
        {
            if ((i != 3 || gDepth) && (i != 4 || gStencilSample) && Ready(gFragments[i]) == false)
            {
                return;
            }
        }
        for (usize i = 0; i < 4; ++i)
        {
            if ((i != 2 || gDepth) && (i != 3 || gStencilSample) && Ready(gLayouts[i]) == false)
            {
                return;
            }
        }
        if (Ready(gSampled[6]) == false)
        {
            return;
        }
        if ((gStencilSample && Ready(gStencilView) == false) || Ready(gSampler) == false || Ready(gVertex) == false)
        {
            return;
        }
        if (PipelinesAndSets() == false)
        {
            Stop(true);
            return;
        }
        gStage = 3;
    }
    if (gStage == 3)
    {
        for (usize i = 0; i < 7; ++i)
        {
            if ((i != 5 || gDepth) && (i != 6 || gStencilSample) &&
                (Ready(gSets[i]) == false || Ready(gPipelines[i]) == false))
            {
                return;
            }
        }
        if (Ready(gSets[7]) == false)
        {
            return;
        }
        if (Ready(gEmpty) == false)
        {
            return;
        }
        gStage = 4;
    }
    if (gStage == 4)
    {
        if (rhi::SetFrameTarget(gDevice, gSurface, {96, 64, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready ||
            DrawCase((gCase & 1) != 0 || gCase == 16) == false ||
            rhi::RequestTextureReadback(gDevice, gTextures[7], {0, 0, 0, 8, 8}, 32, gRead) !=
                rhi::RasterStatus::Pending)
        {
            Stop(true);
            return;
        }
        gStage = 5;
    }
    if (gStage == 5 && Ready(gRead))
    {
        if (Verify() == false || rhi::Release(gDevice, gRead) != rhi::RasterStatus::Ready)
        {
            Stop(true);
            return;
        }
        ++gFrames;
        ++gCase;
        Report(gStage, gFrames, 0, 0);
        if (gCase == 17)
        {
            Stop(false);
            return;
        }
        gStage = 4;
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
    ludus::platform::WindowBase::CreateInfo config;
    config.Width = 96;
    config.Height = 64;
    config.CanvasSelector = "#canvas";
    config.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(config, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Selection = Selection() == 1   ? rhi::BackendSelection::WebGPU
                            : Selection() == 2 ? rhi::BackendSelection::WebGL2
                                               : rhi::BackendSelection::Auto;
    const auto created = rhi::CreateDevice({}, window, description, gDevice, gSurface);
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
