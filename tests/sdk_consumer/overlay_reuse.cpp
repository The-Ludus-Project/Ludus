#include "overlay_reuse.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <initializer_list>
#include <string_view>

namespace ludus::qa
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace renderer = graphics::renderer;
using Status = rhi::RasterStatus;
constexpr logging::LogCategory LOG_OVERLAY_REUSE{"OverlayReuse"};
constexpr uint32 EXTENT = 8;
constexpr usize CHANNEL_COUNT = 4;
constexpr usize ALPHA_CHANNEL = CHANNEL_COUNT - 1;
constexpr usize ROW_BYTES = EXTENT * CHANNEL_COUNT;
constexpr usize IMAGE_BYTES = EXTENT * ROW_BYTES;
constexpr uint8 UNORM_MAX = 255;
constexpr uint8 PARTIAL_COVERAGE = 128;
constexpr usize LAYER_COUNT = 3;
constexpr usize PAIR_COUNT = 2;
constexpr usize TARGET_COUNT = PAIR_COUNT + 1;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize BINDING_COUNT = 2;
using Color = core::StaticArray<float64, CHANNEL_COUNT>;
using Pixels = core::StaticArray<uint8, IMAGE_BYTES>;

enum class Source : uint8
{
    Empty,
    Red,
    Green,
    Blue,
    LowerPair,
    UpperPair,
};

enum class Target : uint8
{
    LowerPair,
    UpperPair,
    Result,
};

struct Layer final
{
    math::Vector4 RectanglePixel;
    Color StraightColor;
    uint8 Coverage;
};

// The overlap has a fractional R8 coverage sample on green, a red-only strip,
// and transparent pixels below the quads. The asymmetric Y extent also detects a sampled Y flip.
constexpr float32 LAYER_TOP_PIXEL = 1;
constexpr float32 LAYER_BOTTOM_PIXEL = 4;
constexpr core::StaticArray<Layer, LAYER_COUNT> LAYERS{{
    {{1, LAYER_TOP_PIXEL, 5, LAYER_BOTTOM_PIXEL}, {{1, 0, 0, 0.5}}, UNORM_MAX},
    {{3, LAYER_TOP_PIXEL, 7, LAYER_BOTTOM_PIXEL}, {{0, 1, 0, 0.75}}, PARTIAL_COVERAGE},
    {{2, LAYER_TOP_PIXEL, 6, LAYER_BOTTOM_PIXEL}, {{0, 0, 1, 0.5}}, UNORM_MAX},
}};

using DrawOrder = core::StaticArray<Source, LAYER_COUNT>;
constexpr DrawOrder LOWER_PAIR{{Source::Red, Source::Green}};
constexpr DrawOrder UPPER_PAIR{{Source::Green, Source::Blue}};
constexpr DrawOrder ALL_LAYERS{{Source::Red, Source::Green, Source::Blue}};
constexpr Color TRANSPARENT{};
constexpr Color CYAN{{0, 1, 1, 1}};
constexpr Color YELLOW{{1, 1, 0, 1}};

enum class Comparison : uint8
{
    Reference,
    MatchDirect,
    ReverseLowerPair,
};

struct CompositionCase final
{
    std::string_view Name;
    Target Destination;
    Color Background;
    DrawOrder Draws;
    DrawOrder ReferenceOrder;
    Comparison Compare = Comparison::Reference;
};

constexpr usize CASE_COUNT = 10;
constexpr core::StaticArray<CompositionCase, CASE_COUNT> CASES{{
    {"red then green intermediate", Target::LowerPair, TRANSPARENT, LOWER_PAIR, LOWER_PAIR},
    {"green then blue intermediate", Target::UpperPair, TRANSPARENT, UPPER_PAIR, UPPER_PAIR},
    {"reversed painter order",
     Target::Result,
     TRANSPARENT,
     {{Source::Green, Source::Red}},
     {{Source::Green, Source::Red}},
     Comparison::ReverseLowerPair},
    {"direct over cyan", Target::Result, CYAN, LOWER_PAIR, LOWER_PAIR},
    {"sampled over cyan", Target::Result, CYAN, {{Source::LowerPair}}, LOWER_PAIR, Comparison::MatchDirect},
    {"direct over yellow", Target::Result, YELLOW, LOWER_PAIR, LOWER_PAIR},
    {"sampled over yellow", Target::Result, YELLOW, {{Source::LowerPair}}, LOWER_PAIR, Comparison::MatchDirect},
    {"three direct layers", Target::Result, TRANSPARENT, ALL_LAYERS, ALL_LAYERS},
    {"group lower two layers",
     Target::Result,
     TRANSPARENT,
     {{Source::LowerPair, Source::Blue}},
     ALL_LAYERS,
     Comparison::MatchDirect},
    {"group upper two layers",
     Target::Result,
     TRANSPARENT,
     {{Source::Red, Source::UpperPair}},
     ALL_LAYERS,
     Comparison::MatchDirect},
}};

enum class Step : uint8
{
    CreateResources,
    CreatePipelineAndBindings,
    WaitForDrawResources,
    Draw,
    ReadPixels,
    Complete,
};

Step gReuseStep = Step::CreateResources;
usize gCaseIndex = 0;
usize gPairBindingCount = 0;
rhi::RasterShaderHandle gVertex;
rhi::RasterShaderHandle gFragment;
rhi::BindingLayoutHandle gLayout;
rhi::SamplerHandle gSampler;
rhi::BufferHandle gVertices;
rhi::BufferHandle gIndices;
rhi::TextureHandle gCoverage;
core::StaticArray<rhi::TextureHandle, TARGET_COUNT> gTargets;
core::StaticArray<rhi::TextureViewHandle, PAIR_COUNT> gPairViews;
core::StaticArray<rhi::BindingSetHandle, PAIR_COUNT> gPairBindings;
core::StaticArray<renderer::PreparedOverlay, LAYER_COUNT> gLayers;
rhi::RasterPipelineHandle gPipeline;
rhi::ReadbackTicket gReadback;
Pixels gDirectPixels;

[[nodiscard]] bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}

[[nodiscard]] Status ReportFailure(std::string_view operation, Status status) noexcept
{
    if (status != Status::Pending && status != Status::NotReady)
    {
        LUDUS_LOG_ERROR(LOG_OVERLAY_REUSE,
                        "{} failed at case {} with raster status {}",
                        operation,
                        gCaseIndex + 1,
                        static_cast<uint32>(status));
    }
    return status;
}

[[nodiscard]] Status GetReadiness(std::initializer_list<Status> statuses) noexcept
{
    Status result = Status::Ready;
    for (const Status status : statuses)
    {
        if (status == Status::Pending || status == Status::NotReady)
        {
            result = Status::Pending;
        }
        else if (status != Status::Ready)
        {
            return status;
        }
    }
    return result;
}

[[nodiscard]] usize GetLayerIndex(Source source) noexcept
{
    return static_cast<usize>(source) - static_cast<usize>(Source::Red);
}

[[nodiscard]] bool CreateResources(rhi::DeviceHandle device) noexcept
{
    constexpr core::StaticArray<math::Vector4, QUAD_VERTEX_COUNT> VERTICES{{
        {-1, 1, 0, 0},
        {1, 1, 1, 0},
        {1, -1, 1, 1},
        {-1, -1, 0, 1},
    }};
    constexpr core::StaticArray<uint32, QUAD_INDEX_COUNT> INDICES{{0, 1, 2, 2, 3, 0}};
    constexpr core::StaticArray<rhi::RasterBinding, BINDING_COUNT> BINDINGS{{
        {0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
        {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
    }};
    constexpr core::StaticArray<uint8, CHANNEL_COUNT> MASK{{PARTIAL_COVERAGE, 0, 0, 0}};
    if (IsAccepted(rhi::CreateRasterShader(device, shaders::renderer_composite::Vertex(), gVertex)) == false ||
        IsAccepted(rhi::CreateRasterShader(device, shaders::renderer_composite::Fragment(), gFragment)) == false ||
        IsAccepted(rhi::CreateBindingLayout(device, BINDINGS, gLayout)) == false ||
        IsAccepted(rhi::CreateSampler(device, {}, gSampler)) == false ||
        IsAccepted(rhi::CreateBuffer(device,
                                     {rhi::BufferRole::Vertex, sizeof(VERTICES)},
                                     {reinterpret_cast<const uint8*>(VERTICES.GetData()), sizeof(VERTICES)},
                                     gVertices)) == false ||
        IsAccepted(rhi::CreateBuffer(device,
                                     {rhi::BufferRole::Index32, sizeof(INDICES)},
                                     {reinterpret_cast<const uint8*>(INDICES.GetData()), sizeof(INDICES)},
                                     gIndices)) == false ||
        IsAccepted(rhi::CreateTexture(device, {1, 1, rhi::RasterFormat::R8Unorm}, {MASK, MASK.GetSize()}, gCoverage)) ==
            false)
    {
        return false;
    }
    for (rhi::TextureHandle& target : gTargets)
    {
        if (IsAccepted(rhi::CreateTexture(device, {EXTENT, EXTENT, rhi::RasterFormat::Rgba8Unorm, true}, {}, target)) ==
            false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool CreateDrawResources(rhi::DeviceHandle device, renderer::OverlayRenderer& inoutOverlays) noexcept
{
    const rhi::RasterVertexStream stream{sizeof(math::Vector4), false};
    const rhi::RasterVertexAttribute attribute{0, 0, 0, rhi::RasterVertexFormat::Float4};
    const rhi::RasterPipelineDescription pipeline
    {
        .Vertex = gVertex,
        .Fragment = gFragment,
        .Layout = gLayout,
        .Streams = {&stream, 1},
        .Attributes = {&attribute, 1},
        .Blend = true,
        .Target = rhi::RasterTarget::Rgba8Unorm,
    };
    renderer::OverlayList list;
    if (IsAccepted(rhi::CreateRasterPipeline(device, pipeline, gPipeline)) == false ||
        list.Initialize() != Status::Ready)
    {
        return false;
    }
    for (usize index = 0; index < LAYER_COUNT; ++index)
    {
        const Layer& layer = LAYERS[index];
        const bool hasPartialCoverage = layer.Coverage != UNORM_MAX;
        const math::Vector4 color{static_cast<float32>(layer.StraightColor[0]),
                                  static_cast<float32>(layer.StraightColor[1]),
                                  static_cast<float32>(layer.StraightColor[2]),
                                  static_cast<float32>(layer.StraightColor[ALPHA_CHANNEL])};
        list.Clear();
        if (list.Quad(layer.RectanglePixel, {0, 0, 1, 1}, color, EXTENT, EXTENT, hasPartialCoverage) != Status::Ready ||
            IsAccepted(
                inoutOverlays.Prepare(list, hasPartialCoverage ? gCoverage : rhi::TextureHandle{}, gLayers[index])) ==
                false)
        {
            return false;
        }
    }
    for (usize index = 0; index < PAIR_COUNT; ++index)
    {
        if (IsAccepted(rhi::CreateTextureView(device, gTargets[index], gPairViews[index])) == false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Status PreparePairBindings(rhi::DeviceHandle device) noexcept
{
    while (gPairBindingCount < PAIR_COUNT)
    {
        const Status view = rhi::GetStatus(device, gPairViews[gPairBindingCount]);
        if (view != Status::Ready)
        {
            return view;
        }
        const core::StaticArray<rhi::RasterBindingResource, BINDING_COUNT> resources{{
            { .Binding = 0, .Texture = gPairViews[gPairBindingCount] },
            { .Binding = 1, .Sampler = gSampler },
        }};
        const Status created = rhi::CreateBindingSet(device, gLayout, resources, gPairBindings[gPairBindingCount]);
        if (IsAccepted(created) == false)
        {
            return created;
        }
        ++gPairBindingCount;
    }
    for (const rhi::BindingSetHandle binding : gPairBindings)
    {
        const Status status = rhi::GetStatus(device, binding);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    return Status::Ready;
}

[[nodiscard]] Status DrawCase(rhi::DeviceHandle device,
                              rhi::SurfaceHandle surface,
                              renderer::OverlayRenderer& inoutOverlays,
                              const CompositionCase& test) noexcept
{
    const rhi::DeviceStatus begun = rhi::BeginFrame(device, surface);
    if (begun == rhi::DeviceStatus::Skipped)
    {
        return Status::Pending;
    }
    if (begun != rhi::DeviceStatus::Ready)
    {
        return Status::Failed;
    }

    rhi::RasterPassDescription pass;
    pass.Color = gTargets[static_cast<usize>(test.Destination)];
    pass.DepthStore = rhi::RasterStore::Store;
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        pass.Clear[channel] = static_cast<float32>(test.Background[channel]);
    }
    Status status = rhi::BeginRasterPass(device, pass);
    for (const Source source : test.Draws)
    {
        if (status != Status::Ready || source == Source::Empty)
        {
            break;
        }
        if (source == Source::LowerPair || source == Source::UpperPair)
        {
            const usize index = static_cast<usize>(source) - static_cast<usize>(Source::LowerPair);
            const rhi::RasterVertexSlice vertices{gVertices, 0, QUAD_VERTEX_COUNT * sizeof(math::Vector4)};
            // The existing composite shader returns the sampled premultiplied RGBA
            // unchanged. The RHI blend applies the source-over destination factor.
            status = rhi::DrawIndexed(
                device,
                {gPipeline, gPairBindings[index], {&vertices, 1}, gIndices, 0, QUAD_INDEX_COUNT, QUAD_VERTEX_COUNT});
        }
        else
        {
            status = inoutOverlays.Draw(gLayers[GetLayerIndex(source)], pass);
        }
    }
    rhi::SubmissionToken completion;
    const Status submitted = rhi::EndFrame(device, completion);
    return status == Status::Ready ? submitted : status;
}

// Thanks to Bruce A. Wallace, "Merging and Transformation of Raster Images for Cartoon
// Animation" (SIGGRAPH '81, p. 257), for reuse of merged color and opacity, and to
// Thomas Porter and Tom Duff, "Compositing Digital Images" (SIGGRAPH '84, pp. 254–255),
// for premultiplied source-over and associative grouping.
// https://graphics.stanford.edu/papers/merging-sig81/wallace-merging-sig81.pdf
// https://doi.org/10.1145/800031.808606
[[nodiscard]] Color ComputeReference(const CompositionCase& test, usize x, usize y) noexcept
{
    Color result = test.Background;
    const float64 centerX = static_cast<float64>(x) + 0.5;
    const float64 centerY = static_cast<float64>(y) + 0.5;
    for (const Source source : test.ReferenceOrder)
    {
        if (source == Source::Empty)
        {
            break;
        }
        const Layer& layer = LAYERS[GetLayerIndex(source)];
        const math::Vector4 bounds = layer.RectanglePixel;
        if (centerX < bounds.X || centerX >= bounds.Z || centerY < bounds.Y || centerY >= bounds.W)
        {
            continue;
        }
        const float64 alpha = layer.StraightColor[ALPHA_CHANNEL] * (static_cast<float64>(layer.Coverage) / UNORM_MAX);
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            const float64 sourceChannel = channel == ALPHA_CHANNEL ? 1 : layer.StraightColor[channel];
            result[channel] = sourceChannel * alpha + result[channel] * (1 - alpha);
        }
    }
    return result;
}

[[nodiscard]] bool CheckPixels(const CompositionCase& test, const Pixels& pixels) noexcept
{
    // Allow one byte per layer write for UNorm rounding and native blend precision.
    // A reused group adds one blend/store. Allow one more byte for its RGBA8 intermediate.
    const int32 layerCount = test.ReferenceOrder.GetLast() == Source::Empty ? PAIR_COUNT : LAYER_COUNT;
    const bool shouldCompareDirect = test.Compare == Comparison::MatchDirect;
    const int32 referenceTolerance = layerCount + (shouldCompareDirect ? 1 : 0);
    // Between GPU routes, nearest UNorm rounding contributes up to half a byte per
    // write in each route. Round the sum (layerCount + 0.5 bytes) up to a whole byte.
    const int32 directTolerance = layerCount + 1;
    for (usize y = 0; y < EXTENT; ++y)
    {
        for (usize x = 0; x < EXTENT; ++x)
        {
            const Color expected = ComputeReference(test, x, y);
            for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
            {
                const usize offset = y * ROW_BYTES + x * CHANNEL_COUNT + channel;
                const float64 expectedByte = expected[channel] * UNORM_MAX;
                const int32 actual = pixels[offset];
                if (actual < expectedByte - referenceTolerance || actual > expectedByte + referenceTolerance)
                {
                    LUDUS_LOG_ERROR(LOG_OVERLAY_REUSE,
                                    "{} pixel ({}, {}) channel {} expected {} byte, got {} (tolerance {})",
                                    test.Name,
                                    x,
                                    y,
                                    channel,
                                    expectedByte,
                                    actual,
                                    referenceTolerance);
                    return false;
                }
                if (shouldCompareDirect == true)
                {
                    const int32 difference = actual - gDirectPixels[offset];
                    if (difference < -directTolerance || difference > directTolerance)
                    {
                        LUDUS_LOG_ERROR(LOG_OVERLAY_REUSE,
                                        "{} pixel ({}, {}) channel {} direct byte {} reused byte {} (tolerance {})",
                                        test.Name,
                                        x,
                                        y,
                                        channel,
                                        gDirectPixels[offset],
                                        actual,
                                        directTolerance);
                        return false;
                    }
                }
            }
        }
    }
    if (test.Compare == Comparison::ReverseLowerPair)
    {
        constexpr usize OVERLAP_X = EXTENT / 2;
        constexpr usize OVERLAP_Y = 2;
        constexpr usize RED = (OVERLAP_Y * EXTENT + OVERLAP_X) * CHANNEL_COUNT;
        constexpr usize GREEN = RED + 1;
        constexpr usize ALPHA = RED + ALPHA_CHANNEL;
        const int32 alphaDifference = static_cast<int32>(pixels[ALPHA]) - gDirectPixels[ALPHA];
        if (pixels[RED] <= gDirectPixels[RED] || pixels[GREEN] >= gDirectPixels[GREEN] ||
            alphaDifference < -layerCount || alphaDifference > layerCount)
        {
            LUDUS_LOG_ERROR(LOG_OVERLAY_REUSE,
                            "Reversed overlap must increase red, decrease green, and preserve alpha");
            return false;
        }
    }
    // Keep the lower pair until its reverse control, then retain each direct
    // result for the immediately following reuse cases.
    if (test.Destination == Target::LowerPair ||
        (test.Destination == Target::Result && test.Compare == Comparison::Reference))
    {
        gDirectPixels = pixels;
    }
    return true;
}

} // namespace

Status AdvanceOverlayReuse(rhi::DeviceHandle device,
                           rhi::SurfaceHandle surface,
                           renderer::OverlayRenderer& inoutOverlays) noexcept
{
    const Status lifetime = rhi::PollLifetime(device);
    if (lifetime != Status::Ready)
    {
        return ReportFailure("polling lifetime", lifetime);
    }
    if (gReuseStep == Step::CreateResources)
    {
        if (CreateResources(device) == false)
        {
            return ReportFailure("creating resources", Status::Failed);
        }
        gReuseStep = Step::CreatePipelineAndBindings;
    }
    if (gReuseStep == Step::CreatePipelineAndBindings)
    {
        const Status ready = GetReadiness({rhi::GetStatus(device, gVertex),
                                           rhi::GetStatus(device, gFragment),
                                           rhi::GetStatus(device, gLayout),
                                           rhi::GetStatus(device, gSampler),
                                           rhi::GetStatus(device, gCoverage),
                                           rhi::GetStatus(device, gVertices),
                                           rhi::GetStatus(device, gIndices),
                                           rhi::GetStatus(device, gTargets[0]),
                                           rhi::GetStatus(device, gTargets[1]),
                                           rhi::GetStatus(device, gTargets[2])});
        if (ready != Status::Ready)
        {
            return ReportFailure("waiting for initial resources", ready);
        }
        if (CreateDrawResources(device, inoutOverlays) == false)
        {
            return ReportFailure("creating draw resources", Status::Failed);
        }
        gReuseStep = Step::WaitForDrawResources;
    }
    if (gReuseStep == Step::WaitForDrawResources)
    {
        const Status ready = GetReadiness({rhi::GetStatus(device, gPipeline),
                                           PreparePairBindings(device),
                                           inoutOverlays.GetStatus(gLayers[0]),
                                           inoutOverlays.GetStatus(gLayers[1]),
                                           inoutOverlays.GetStatus(gLayers[2])});
        if (ready != Status::Ready)
        {
            return ReportFailure("waiting for draw resources", ready);
        }
        gReuseStep = Step::Draw;
    }
    if (gReuseStep == Step::Complete)
    {
        return Status::Ready;
    }

    const CompositionCase& test = CASES[gCaseIndex];
    if (gReuseStep == Step::Draw)
    {
        const Status drawn = DrawCase(device, surface, inoutOverlays, test);
        if (drawn != Status::Ready)
        {
            return ReportFailure(test.Name, drawn);
        }
        const Status requested = rhi::RequestTextureReadback(device,
                                                             gTargets[static_cast<usize>(test.Destination)],
                                                             { .Width = EXTENT, .Height = EXTENT },
                                                             ROW_BYTES,
                                                             gReadback);
        if (IsAccepted(requested) == false)
        {
            return ReportFailure("requesting readback", requested);
        }
        gReuseStep = Step::ReadPixels;
    }

    Pixels pixels;
    const Status copied = rhi::CopyReadback(device, gReadback, pixels.GetData(), pixels.GetSize());
    if (copied != Status::Ready)
    {
        return ReportFailure("copying readback", copied);
    }
    const Status released = rhi::Release(device, gReadback);
    if (released != Status::Ready)
    {
        return ReportFailure("releasing readback", released);
    }
    if (CheckPixels(test, pixels) == false)
    {
        return Status::Failed;
    }
    ++gCaseIndex;
    if (gCaseIndex == CASES.GetSize())
    {
        LUDUS_LOG_INFO(LOG_OVERLAY_REUSE,
                       "{} offscreen cases passed on backend {}",
                       gCaseIndex,
                       static_cast<uint32>(rhi::GetStartup().SelectedBackend));
        gReuseStep = Step::Complete;
        return Status::Ready;
    }
    gReuseStep = Step::Draw;
    return Status::Pending;
}
} // namespace ludus::qa
