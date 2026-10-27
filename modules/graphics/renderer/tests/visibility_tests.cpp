#include "../src/internal/gpu_preview_ray.h"
#include "../src/internal/render_graph_ray.h"
#include "renderer_authored.h"
#include "renderer_authored_shadow.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_gpu_preview.h"
#include "renderer_gpu_preview_hit_cached.h"
#include "renderer_gpu_preview_image.h"
#include "renderer_gpu_preview_visibility_full.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/measurement.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
using Status = rhi::RasterStatus;
constexpr uint64 TIMEOUT = 30 * time::NANOSECONDS_PER_SECOND;
constexpr usize BYTE_BUDGET = usize{64} * 1024 * 1024;
constexpr usize VARIANT_COUNT = 2;
constexpr usize EXPECTED_PASS_COUNT = 2;
constexpr usize TRANSPORT_PASS_INDEX = 0;
constexpr usize OUTPUT_PASS_INDEX = 1;
constexpr usize PROFILED_VARIANT_INDEX = 1;
constexpr usize PASS_METADATA_WORD_COUNT = 7;
constexpr usize PASS_INTERVAL_WORD_COUNT = 6;
constexpr uint64 PASS_METADATA_VERSION = 1;
constexpr usize SCENE_COUNT = 4;
constexpr usize ALL_SCENE_COUNT = 6;
constexpr uint32 TEXTURE_EXTENT = 4;
constexpr float32 TEXTURE_UV_REPETITIONS = 3;
constexpr uint8 TEXEL_HIGH = 224;
constexpr uint8 TEXEL_LOW = 64;
constexpr uint8 TEXEL_MIDDLE = 128;
constexpr uint8 TEXEL_MAXIMUM = 255;
constexpr uint32 GRID_CELLS = 32;
constexpr usize TRIANGLE_CORNERS = 3;
constexpr usize QUAD_VERTICES = 2 * TRIANGLE_CORNERS;
constexpr usize MAX_VERTICES = usize(GRID_CELLS) * GRID_CELLS * QUAD_VERTICES;
constexpr uint32 TEST_EXTENT = 64;
constexpr uint32 TEST_SAMPLES = 16;
constexpr uint32 BENCHMARK_EXTENT = 512;
constexpr uint32 BENCHMARK_SAMPLES = 128;
constexpr uint32 WARMUP_SAMPLES = 16;
constexpr usize COLOR_CHANNELS = 4;
constexpr usize ACCUMULATION_WORDS = 2;
constexpr usize READBACK_ROWS = 16;
constexpr usize READBACK_CHUNK_BYTES = usize{64} * 1024;
constexpr float32 RECEIVER_DISTANCE_WORLD = .75F;
constexpr float32 REJECTING_DISTANCE_WORLD = .25F;
constexpr float32 LIGHT_DIRECTION_X = .6F;
constexpr float32 LIGHT_DIRECTION_Z = .8F;
constexpr float32 ENVIRONMENT_RED = .25F;
constexpr float32 LINEAR_ABSOLUTE_TOLERANCE = .001F;
constexpr float32 LINEAR_RELATIVE_TOLERANCE = .005F;
constexpr usize CONFIGURATION_WORD_COUNT = 4;
constexpr uint32 BENCHMARK_FORMAT_VERSION = 1;
constexpr StaticArray<const char*, ALL_SCENE_COUNT> SCENE_NAMES{
    {"empty", "opaque", "masked", "mixed", "textured-nearest", "textured-linear"}};
constexpr StaticArray<const char*, VARIANT_COUNT> CACHE_VARIANT_NAMES{{"uncached", "cached"}};
constexpr StaticArray<const char*, VARIANT_COUNT> VARIANT_NAMES{{"full", "visibility"}};
constexpr StaticArray<const char*, VARIANT_COUNT> PASS_VARIANT_NAMES{{"unprobed", "profiled"}};
enum class BenchmarkExperiment : uint32
{
    Visibility,
    HitCache,
    PassTimings,
};
enum class SceneKind : uint32
{
    Empty,
    Opaque,
    Masked,
    Mixed,
    TexturedNearest,
    TexturedLinear,
};
enum class RenderPath : uint32
{
    FullHit,
    VisibilityOnly,
    UncachedHit,
    CachedHit,
    CpuOracle,
    GpuReference,
};
struct VisibilityFixture final
{
    rr::Renderer Renderer;
    rr::Snapshot Snapshot;
    rr::GpuPreview Preview;
};
struct PassTimingSamples final
{
    StaticArray<Array<float64>, EXPECTED_PASS_COUNT> Nanoseconds;
    Array<uint64> IntervalWords;
    StaticArray<rhi::GpuPassTiming, EXPECTED_PASS_COUNT> Metadata{};
    bool HasMetadata = false;
};
struct TimingSamples final
{
    PassTimingSamples Passes;
    StaticArray<Array<float64>, VARIANT_COUNT> GpuNanoseconds;
    StaticArray<Array<uint64>, VARIANT_COUNT> WallNanoseconds;
    Array<uint64> PairGapNanoseconds;
    Array<uint8> IsBaselineFirst;
};
bool IsAccepted(Status status)
{
    return status == Status::Ready || status == Status::Pending;
}
template <typename Handle>
void WaitReady(rr::Renderer& inoutRenderer, Handle handle)
{
    const uint64 start = time::NowTicks();
    Status status = inoutRenderer.GetStatus(handle);
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
        status = inoutRenderer.GetStatus(handle);
    REQUIRE(status == Status::Ready);
}
void WaitSamples(VisibilityFixture& inoutFixture, uint32 targetSamples)
{
    const uint64 start = time::NowTicks();
    rr::GpuPreviewInfo information;
    Status status = Status::Pending;
    do
    {
        status = inoutFixture.Renderer.GetGpuPreviewInfo(inoutFixture.Preview, information);
        REQUIRE(IsAccepted(status) == true);
    } while ((status == Status::Pending || information.CompletedSamples < targetSamples) &&
             time::NowTicks() - start < TIMEOUT);
    REQUIRE(information.CompletedSamples == targetSamples);
}
void CreateMesh(rr::Renderer& inoutRenderer, bool shouldKeepEvenColumns, rr::Mesh& outMesh)
{
    const usize vertexCount = shouldKeepEvenColumns == true ? MAX_VERTICES / 2 : MAX_VERTICES;
    Array<math::Vector3> positionsMesh;
    Array<math::Vector3> normalsMesh;
    Array<math::Vector2> texcoords;
    Array<uint32> indices;
    REQUIRE(positionsMesh.TryResize(vertexCount) == true);
    REQUIRE(normalsMesh.TryResize(vertexCount) == true);
    REQUIRE(texcoords.TryResize(vertexCount) == true);
    REQUIRE(indices.TryResize(vertexCount) == true);
    usize vertex = 0;
    for (uint32 y = 0; y < GRID_CELLS; ++y)
    {
        for (uint32 x = 0; x < GRID_CELLS; ++x)
        {
            if (shouldKeepEvenColumns == true && x % 2 != 0)
                continue;
            const float32 left = float32(x) * 2 / float32(GRID_CELLS) - 1;
            const float32 right = float32(x + 1) * 2 / float32(GRID_CELLS) - 1;
            const float32 bottom = float32(y) * 2 / float32(GRID_CELLS) - 1;
            const float32 top = float32(y + 1) * 2 / float32(GRID_CELLS) - 1;
            const StaticArray<math::Vector3, QUAD_VERTICES> cornersMesh{{{left, bottom, 0},
                                                                         {right, top, 0},
                                                                         {right, bottom, 0},
                                                                         {left, bottom, 0},
                                                                         {left, top, 0},
                                                                         {right, top, 0}}};
            for (const math::Vector3& cornerMesh : cornersMesh)
            {
                positionsMesh[vertex] = cornerMesh;
                normalsMesh[vertex] = {0, 0, -1};
                texcoords[vertex] = {cornerMesh.X * .5F + .5F, cornerMesh.Y * .5F + .5F};
                indices[vertex] = static_cast<uint32>(vertex);
                ++vertex;
            }
        }
    }
    REQUIRE(vertex == vertexCount);
    rr::MeshDescription description{positionsMesh.GetData(), vertexCount, indices.GetData(), vertexCount};
    description.Normals = normalsMesh.GetData();
    description.Texcoords = texcoords.GetData();
    REQUIRE(IsAccepted(inoutRenderer.CreateMesh(description, outMesh)) == true);
    WaitReady(inoutRenderer, outMesh);
}
void PrepareFixture(rhi::DeviceHandle device, RenderPath path, SceneKind scene, VisibilityFixture& outFixture)
{
    rr::Renderer& renderer = outFixture.Renderer;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            Status::Ready);
    if (path == RenderPath::FullHit || path == RenderPath::VisibilityOnly || path == RenderPath::UncachedHit ||
        path == RenderPath::CachedHit)
    {
        const rhi::RasterShaderDescription shader =
            path == RenderPath::CachedHit ? shaders::renderer_gpu_preview_hit_cached::Compute()
            : path == RenderPath::FullHit ? shaders::renderer_gpu_preview_visibility_full::Compute()
                                          : shaders::renderer_gpu_preview::Compute();
        REQUIRE(renderer.EnableGpuPreview(shader,
                                          shaders::renderer_gpu_preview_image::Vertex(),
                                          shaders::renderer_gpu_preview_image::Fragment(),
                                          {BYTE_BUDGET, BYTE_BUDGET, 1}) == Status::Ready);
    }
    else
    {
        rr::ReferenceRenderingOptions options;
        options.Traversal =
            path == RenderPath::CpuOracle ? rr::ReferenceTraversal::CpuOracle : rr::ReferenceTraversal::GpuCompute;
        options.CpuByteBudget = BYTE_BUDGET;
        options.GpuByteBudget = BYTE_BUDGET;
        options.TilePixels = TEST_EXTENT;
        const Status enabled = path == RenderPath::CpuOracle
                                   ? renderer.EnableReferenceRendering({}, options)
                                   : renderer.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                                       shaders::renderer_reference_reduce::Compute(),
                                                                       options);
        REQUIRE(enabled == Status::Ready);
    }
    REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            Status::Ready);
    REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                        shaders::renderer_output::Fragment(),
                                        shaders::renderer_shadow::Fragment()) == Status::Ready);
    if (scene == SceneKind::Empty)
    {
        REQUIRE(renderer.CreateSnapshot(nullptr, 0, outFixture.Snapshot) == Status::Ready);
        return;
    }
    const bool isTextured = scene == SceneKind::TexturedNearest || scene == SceneKind::TexturedLinear;
    rr::MaterialProgram program;
    REQUIRE(IsAccepted(renderer.PrewarmLitProgram(
                isTextured == true ? shaders::renderer_authored::Vertex() : shaders::renderer_lit::Vertex(),
                isTextured == true ? shaders::renderer_authored::Fragment() : shaders::renderer_lit::Fragment(),
                isTextured == true ? shaders::renderer_authored_shadow::Fragment()
                                   : shaders::renderer_lit_shadow::Fragment(),
                program)) == true);
    WaitReady(renderer, program);
    rr::Mesh receiver;
    rr::Mesh occluder;
    CreateMesh(renderer, false, receiver);
    CreateMesh(renderer, true, occluder);
    rhi::TextureHandle baseTexture;
    rhi::TextureHandle emissionTexture;
    rhi::TextureHandle surfaceTexture;
    if (isTextured == true)
    {
        StaticArray<uint8, TEXTURE_EXTENT * TEXTURE_EXTENT * COLOR_CHANNELS> base{};
        StaticArray<uint8, TEXTURE_EXTENT * TEXTURE_EXTENT * COLOR_CHANNELS> emission{};
        StaticArray<uint8, TEXTURE_EXTENT * TEXTURE_EXTENT * COLOR_CHANNELS> surface{};
        for (usize y = 0; y < TEXTURE_EXTENT; ++y)
        {
            for (usize x = 0; x < TEXTURE_EXTENT; ++x)
            {
                const usize offset = (y * TEXTURE_EXTENT + x) * COLOR_CHANNELS;
                base[offset] = (x + y) % 2 == 0 ? TEXEL_HIGH : TEXEL_LOW;
                base[offset + 1] = TEXEL_MIDDLE;
                base[offset + 2] = x % 2 == 0 ? TEXEL_LOW : TEXEL_HIGH;
                base[offset + COLOR_CHANNELS - 1] = TEXEL_MAXIMUM;
                emission[offset] = y % 2 == 0 ? TEXEL_HIGH : TEXEL_LOW;
                emission[offset + 1] = TEXEL_LOW;
                emission[offset + 2] = TEXEL_MIDDLE;
                emission[offset + COLOR_CHANNELS - 1] = TEXEL_MAXIMUM;
                surface[offset] = TEXEL_MIDDLE;
                surface[offset + 1] = TEXEL_MIDDLE;
                surface[offset + 2] = x % 2 == 0 ? TEXEL_HIGH : TEXEL_MIDDLE;
                surface[offset + COLOR_CHANNELS - 1] = TEXEL_LOW;
            }
        }
        rhi::TextureDescription textureDescription{TEXTURE_EXTENT, TEXTURE_EXTENT, rhi::RasterFormat::Rgba8Srgb};
        textureDescription.RetainCpuContent = true;
        const usize rowBytes = TEXTURE_EXTENT * COLOR_CHANNELS;
        REQUIRE(
            rhi::CreateTexture(device, textureDescription, {{base.GetData(), base.GetSize()}, rowBytes}, baseTexture) ==
            Status::Ready);
        REQUIRE(rhi::CreateTexture(device,
                                   textureDescription,
                                   {{emission.GetData(), emission.GetSize()}, rowBytes},
                                   emissionTexture) == Status::Ready);
        textureDescription.Format = rhi::RasterFormat::Rgba8Unorm;
        REQUIRE(rhi::CreateTexture(device,
                                   textureDescription,
                                   {{surface.GetData(), surface.GetSize()}, rowBytes},
                                   surfaceTexture) == Status::Ready);
    }
    rr::LitMaterialDescription material;
    material.Roughness = 1;
    material.AreAuthoredInputsEnabled = isTextured;
    if (isTextured == true)
    {
        material.Surface.Texture = baseTexture;
        material.EmissionMap = emissionTexture;
        material.SurfaceMap = surfaceTexture;
        material.Emission = {.5F, .5F, .5F};
        material.Metallic = .5F;
        material.Surface.UvTransform = {TEXTURE_UV_REPETITIONS, -TEXTURE_UV_REPETITIONS, .5F, .5F};
        material.Surface.Sampler.Filter =
            scene == SceneKind::TexturedLinear ? rhi::RasterFilter::Linear : rhi::RasterFilter::Nearest;
        material.Surface.Sampler.U = rhi::RasterAddress::Repeat;
        material.Surface.Sampler.V = rhi::RasterAddress::Clamp;
    }
    material.Surface.Tint = {.5F, 1, .5F, 1};
    material.Surface.Alpha = scene == SceneKind::Masked ? rr::MaterialAlpha::Masked : rr::MaterialAlpha::Opaque;
    rr::MaterialVersion receiverMaterial;
    rr::LightSelection selection;
    REQUIRE(IsAccepted(renderer.CreateLitMaterial(program, material, receiverMaterial, selection)) == true);
    WaitReady(renderer, receiverMaterial);
    material.Surface.Tint = {.5F, .5F, 1, 1};
    material.Surface.Alpha = scene == SceneKind::Opaque ? rr::MaterialAlpha::Opaque : rr::MaterialAlpha::Masked;
    rr::MaterialVersion occluderMaterial;
    REQUIRE(IsAccepted(renderer.CreateLitMaterial(program, material, occluderMaterial, selection)) == true);
    WaitReady(renderer, occluderMaterial);
    material.Emission = {};
    material.Surface.Tint = {1, .5F, .5F, 0};
    material.Surface.Alpha = rr::MaterialAlpha::Masked;
    rr::MaterialVersion rejectingMaterial;
    REQUIRE(IsAccepted(renderer.CreateLitMaterial(program, material, rejectingMaterial, selection)) == true);
    WaitReady(renderer, rejectingMaterial);
    constexpr usize ITEM_COUNT = 3;
    StaticArray<rr::SceneItem, ITEM_COUNT> items{};
    items[0].SourceId = 1;
    items[0].Geometry = receiver;
    items[0].Surface = receiverMaterial;
    items[0].Transform.Translation.Z = RECEIVER_DISTANCE_WORLD;
    items[1].SourceId = 2;
    items[1].Geometry = occluder;
    items[1].Surface = occluderMaterial;
    items[1].Transform.Translation.Z = .5F;
    items[2].SourceId = ITEM_COUNT;
    items[2].Geometry = receiver;
    items[2].Surface = rejectingMaterial;
    items[2].Transform.Translation.Z = REJECTING_DISTANCE_WORLD;
    REQUIRE(renderer.CreateSnapshot(items.GetData(),
                                    scene == SceneKind::Opaque ? 2 : items.GetSize(),
                                    outFixture.Snapshot) == Status::Ready);
}
// The fixture owns exactly one transport dispatch and one output raster pass.
// Reject unexpected topology or invalid timestamps instead of attributing other work.
void CollectPassTimings(rhi::DeviceHandle device, usize sampleIndex, PassTimingSamples& inoutSamples)
{
    rhi::GpuPassTimings batch;
    const uint64 start = time::NowTicks();
    do
    {
        REQUIRE(rhi::GetGpuPassTimings(device, batch) == Status::Ready);
    } while (batch.State == rhi::GpuTimingState::Pending && time::NowTicks() - start < TIMEOUT);
    REQUIRE(batch.State == rhi::GpuTimingState::Available);
    REQUIRE(batch.DroppedPasses == 0);
    REQUIRE(batch.Count == EXPECTED_PASS_COUNT);
    StaticArray<bool, EXPECTED_PASS_COUNT> hasSeen{};
    StaticArray<rhi::GpuPassTiming, EXPECTED_PASS_COUNT> observed{};
    for (usize index = 0; index < batch.Count; ++index)
    {
        const rhi::GpuPassTiming& pass = batch.Passes[index];
        REQUIRE((pass.Kind == rhi::GpuPassKind::Compute || pass.Kind == rhi::GpuPassKind::Raster));
        const usize stage = pass.Kind == rhi::GpuPassKind::Compute ? TRANSPORT_PASS_INDEX : OUTPUT_PASS_INDEX;
        REQUIRE(hasSeen[stage] == false);
        hasSeen[stage] = true;
        observed[stage] = pass;
        REQUIRE(pass.Scope.Identity != 0);
        REQUIRE(pass.Scope.Name[0] != '\0');
        REQUIRE(pass.Scope.LogicalPassCount == 1);
        REQUIRE(pass.ScopePassIndex == 0);
        REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
        REQUIRE(math::IsFinite(pass.Timing.DurationNanoseconds) == true);
        REQUIRE(pass.Timing.DurationNanoseconds >= 0);
        REQUIRE(math::IsFinite(pass.Timing.TickNanoseconds) == true);
        REQUIRE(pass.Timing.TickNanoseconds >= 0);
        if (inoutSamples.HasMetadata == true)
        {
            REQUIRE(pass.Scope.Identity == inoutSamples.Metadata[stage].Scope.Identity);
            REQUIRE(std::strcmp(pass.Scope.Name.GetData(), inoutSamples.Metadata[stage].Scope.Name.GetData()) == 0);
            REQUIRE(pass.Timing.TickNanoseconds == inoutSamples.Metadata[stage].Timing.TickNanoseconds);
            REQUIRE(pass.Timing.ValidBits == inoutSamples.Metadata[stage].Timing.ValidBits);
        }
        else
            inoutSamples.Metadata[stage] = pass;
        inoutSamples.Nanoseconds[stage][sampleIndex] = pass.Timing.DurationNanoseconds;
    }
    REQUIRE(inoutSamples.Metadata[TRANSPORT_PASS_INDEX].Scope.Identity !=
            inoutSamples.Metadata[OUTPUT_PASS_INDEX].Scope.Identity);
    const rhi::GpuPassTiming& transport = observed[TRANSPORT_PASS_INDEX];
    const rhi::GpuPassTiming& output = observed[OUTPUT_PASS_INDEX];
    const bool hasAlignment = transport.HasCpuInterval == true && output.HasCpuInterval == true;
    if (hasAlignment == true)
    {
        REQUIRE(transport.EndTicks >= transport.StartTicks);
        REQUIRE(output.EndTicks >= output.StartTicks);
    }
    // Missing calibration does not invalidate GPU durations. Only the aligned
    // intervals are absent, and the explicit final word distinguishes that state.
    const StaticArray<uint64, PASS_INTERVAL_WORD_COUNT> interval{
        {hasAlignment == true ? transport.StartTicks : 0,
         hasAlignment == true ? transport.EndTicks : 0,
         hasAlignment == true ? output.StartTicks : 0,
         hasAlignment == true ? output.EndTicks : 0,
         hasAlignment == true ? batch.ClockUncertaintyNanoseconds : 0,
         hasAlignment == true ? 1U : 0U}};
    for (usize word = 0; word < interval.GetSize(); ++word)
        inoutSamples.IntervalWords[sampleIndex * PASS_INTERVAL_WORD_COUNT + word] = interval[word];
    inoutSamples.HasMetadata = true;
}
void AdvancePairs(rhi::DeviceHandle device,
                  rhi::SurfaceHandle surface,
                  StaticArray<VisibilityFixture, VARIANT_COUNT>& inoutFixtures,
                  uint32 samples,
                  bool isBenchmark,
                  bool shouldMeasurePasses,
                  bool shouldReverseOrder,
                  TimingSamples& outTimings)
{
    const uint32 warmup = isBenchmark == true ? WARMUP_SAMPLES : 0;
    const bool shouldTimeFrames = isBenchmark == true || shouldMeasurePasses == true;
    outTimings.Passes.HasMetadata = false;
    if (shouldMeasurePasses == true)
    {
        for (Array<float64>& values : outTimings.Passes.Nanoseconds)
            REQUIRE(values.TryResize(samples - warmup) == true);
        REQUIRE(outTimings.Passes.IntervalWords.TryResize(usize(samples - warmup) * PASS_INTERVAL_WORD_COUNT) == true);
    }
    if (shouldTimeFrames == true)
    {
        for (usize variant = 0; variant < VARIANT_COUNT; ++variant)
        {
            REQUIRE(outTimings.GpuNanoseconds[variant].TryResize(samples - warmup) == true);
            REQUIRE(outTimings.WallNanoseconds[variant].TryResize(samples - warmup) == true);
        }
        REQUIRE(outTimings.PairGapNanoseconds.TryResize(samples - warmup) == true);
        REQUIRE(outTimings.IsBaselineFirst.TryResize(samples - warmup) == true);
    }
    for (uint32 sample = 1; sample <= samples; ++sample)
    {
        const bool isBaselineFirst = (sample % 2 == 0) != shouldReverseOrder;
        uint64 firstCompletedAt = 0;
        for (usize turn = 0; turn < VARIANT_COUNT; ++turn)
        {
            const usize variant = isBaselineFirst == true ? turn : VARIANT_COUNT - 1 - turn;
            VisibilityFixture& fixture = inoutFixtures[variant];
            if (shouldTimeFrames == true)
                REQUIRE(rhi::ArmGpuTiming(device) == Status::Ready);
            const bool hasPassProbe = shouldMeasurePasses == true && variant == PROFILED_VARIANT_INDEX;
            if (hasPassProbe == true)
            {
                const Status armed = rhi::ArmGpuPassTimings(device);
                if (armed == Status::Unsupported && isBenchmark == false)
                    SKIP("Native per-pass timestamp sampling unavailable");
                INFO("Per-pass timestamps are required in pass-timings benchmark mode");
                REQUIRE(armed == Status::Ready);
            }
            const uint64 startedAt = time::NowTicks();
            rhi::SubmissionToken token;
            REQUIRE(fixture.Renderer.AdvanceGpuPreview(fixture.Preview, surface, token) == Status::Ready);
            WaitSamples(fixture, sample);
            const uint64 completedAt = time::NowTicks();
            if (shouldTimeFrames == true)
            {
                rhi::GpuTiming timing;
                REQUIRE(rhi::GetGpuTiming(device, timing) == Status::Ready);
                REQUIRE(timing.State == rhi::GpuTimingState::Available);
                REQUIRE(math::IsFinite(timing.DurationNanoseconds) == true);
                REQUIRE(timing.DurationNanoseconds > 0);
                if (sample > warmup)
                {
                    outTimings.GpuNanoseconds[variant][sample - warmup - 1] = timing.DurationNanoseconds;
                    outTimings.WallNanoseconds[variant][sample - warmup - 1] = completedAt - startedAt;
                    if (turn == 1)
                    {
                        outTimings.PairGapNanoseconds[sample - warmup - 1] = startedAt - firstCompletedAt;
                        outTimings.IsBaselineFirst[sample - warmup - 1] = isBaselineFirst == true ? 1 : 0;
                    }
                }
            }
            if (hasPassProbe == true)
                CollectPassTimings(device, sample > warmup ? usize(sample - warmup - 1) : 0, outTimings.Passes);
            firstCompletedAt = completedAt;
        }
    }
}
void ReadOutput(rhi::DeviceHandle device,
                VisibilityFixture& inoutFixture,
                uint32 samples,
                Array<uint8>& outPixels,
                Array<math::Vector4>& outLinear)
{
    rr::GpuPreviewFrame frame;
    rr::GpuPreviewImage image;
    REQUIRE(inoutFixture.Renderer.AcquireGpuPreviewFrame(inoutFixture.Preview, frame, image) == Status::Ready);
    const usize rowBytes = usize(image.Width) * COLOR_CHANNELS;
    REQUIRE(outPixels.TryResize(rowBytes * image.Height) == true);
    for (uint32 y = 0; y < image.Height; y += READBACK_ROWS)
    {
        const uint32 rows = math::Min(uint32(READBACK_ROWS), image.Height - y);
        rhi::ReadbackTicket ticket;
        REQUIRE(
            IsAccepted(
                rhi::RequestTextureReadback(device, image.Texture, {0, 0, y, image.Width, rows}, rowBytes, ticket)) ==
            true);
        Status status = rhi::GetStatus(device, ticket);
        const uint64 start = time::NowTicks();
        while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
            status = rhi::GetStatus(device, ticket);
        REQUIRE(status == Status::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, outPixels.GetData() + usize(y) * rowBytes, rows * rowBytes) ==
                Status::Ready);
        REQUIRE(rhi::Release(device, ticket) == Status::Ready);
    }
    rhi::BufferHandle accumulation;
    REQUIRE(rr::internal::RenderGraphRayPass::GetAccumulationForTesting(inoutFixture.Renderer,
                                                                        inoutFixture.Preview,
                                                                        accumulation) == Status::Ready);
    const usize pixelCount = usize(image.Width) * image.Height;
    REQUIRE(outLinear.TryResize(pixelCount * ACCUMULATION_WORDS) == true);
    const usize totalBytes = outLinear.GetSize() * sizeof(math::Vector4);
    for (usize offset = 0; offset < totalBytes; offset += READBACK_CHUNK_BYTES)
    {
        const usize bytes = math::Min(READBACK_CHUNK_BYTES, totalBytes - offset);
        rhi::ReadbackTicket ticket;
        Status status = rhi::RequestBufferReadback(device, accumulation, offset, bytes, ticket);
        const uint64 start = time::NowTicks();
        while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
            status = rhi::GetStatus(device, ticket);
        REQUIRE(status == Status::Ready);
        REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(outLinear.GetData()) + offset, bytes) ==
                Status::Ready);
        REQUIRE(rhi::Release(device, ticket) == Status::Ready);
    }
    for (usize pixel = 0; pixel < pixelCount; ++pixel)
    {
        math::Vector4& value = outLinear[pixel * ACCUMULATION_WORDS];
        REQUIRE(value.W == float32(samples));
        value.X /= value.W;
        value.Y /= value.W;
        value.Z /= value.W;
    }
    REQUIRE(inoutFixture.Renderer.Release(frame) == Status::Ready);
}
void WriteArtifact(const std::string& directory, const char* scene, const char* suffix, const void* data, usize bytes)
{
    REQUIRE(data != nullptr);
    const std::string path = directory + "/" + scene + suffix;
    std::FILE* fileOrNull = std::fopen(path.c_str(), "wb");
    REQUIRE(fileOrNull != nullptr);
    REQUIRE(std::fwrite(data, 1, bytes, fileOrNull) == bytes);
    REQUIRE(std::fclose(fileOrNull) == 0);
}
template <typename Value>
void WriteIntegers(const std::string& directory, const char* scene, const char* suffix, const Array<Value>& values)
{
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(values.GetSize() * sizeof(Value)) == true);
    for (usize i = 0; i < values.GetSize(); ++i)
        REQUIRE(TryWriteLittleEndian(values[i], {bytes.GetData() + i * sizeof(Value), sizeof(Value)}) == true);
    WriteArtifact(directory, scene, suffix, bytes.GetData(), bytes.GetSize());
}
void WriteTimings(const std::string& directory,
                  const char* scene,
                  const Array<float64>& values,
                  const char* suffix = ".gpu.bin")
{
    Array<uint64> bits;
    REQUIRE(bits.TryResize(values.GetSize()) == true);
    for (usize i = 0; i < values.GetSize(); ++i)
        std::memcpy(&bits[i], &values[i], sizeof(uint64));
    WriteIntegers(directory, scene, suffix, bits);
}
void WritePassTimings(const std::string& directory, const char* scene, const PassTimingSamples& samples)
{
    REQUIRE(samples.HasMetadata == true);
    WriteTimings(directory, scene, samples.Nanoseconds[TRANSPORT_PASS_INDEX], ".transport.gpu.bin");
    WriteTimings(directory, scene, samples.Nanoseconds[OUTPUT_PASS_INDEX], ".output.gpu.bin");
    WriteIntegers(directory, scene, ".pass-intervals.bin", samples.IntervalWords);
    const rhi::GpuPassTiming& transport = samples.Metadata[TRANSPORT_PASS_INDEX];
    const rhi::GpuPassTiming& output = samples.Metadata[OUTPUT_PASS_INDEX];
    // Seven explicit little-endian 64-bit words: version, two binary64 ticks,
    // two valid-bit counts and two scope identities. No host struct padding.
    StaticArray<uint64, PASS_METADATA_WORD_COUNT> metadata{{PASS_METADATA_VERSION,
                                                            0,
                                                            0,
                                                            transport.Timing.ValidBits,
                                                            output.Timing.ValidBits,
                                                            transport.Scope.Identity,
                                                            output.Scope.Identity}};
    std::memcpy(&metadata[1], &transport.Timing.TickNanoseconds, sizeof(float64));
    std::memcpy(&metadata[2], &output.Timing.TickNanoseconds, sizeof(float64));
    StaticArray<uint8, PASS_METADATA_WORD_COUNT * sizeof(uint64)> bytes{};
    for (usize index = 0; index < metadata.GetSize(); ++index)
        REQUIRE(TryWriteLittleEndian(metadata[index], {bytes.GetData() + index * sizeof(uint64), sizeof(uint64)}) ==
                true);
    WriteArtifact(directory, scene, ".pass-info.bin", bytes.GetData(), bytes.GetSize());
    StaticArray<uint8, EXPECTED_PASS_COUNT * (sizeof(uint32) + rhi::GPU_SCOPE_NAME_BYTES)> names{};
    usize offset = 0;
    for (const rhi::GpuPassTiming& pass : samples.Metadata)
    {
        uint32 length = 0;
        while (length < pass.Scope.Name.GetSize() && pass.Scope.Name[length] != '\0')
            ++length;
        REQUIRE(length != 0);
        REQUIRE(length < pass.Scope.Name.GetSize());
        REQUIRE(TryWriteLittleEndian(length, {names.GetData() + offset, sizeof(uint32)}) == true);
        offset += sizeof(uint32);
        std::memcpy(names.GetData() + offset, pass.Scope.Name.GetData(), length);
        offset += length;
    }
    WriteArtifact(directory, scene, ".pass-names.bin", names.GetData(), offset);
}
void WriteLinear(const std::string& directory, const char* scene, const Array<math::Vector4>& values)
{
    Array<uint32> bits;
    REQUIRE(bits.TryResize(values.GetSize() * COLOR_CHANNELS) == true);
    for (usize i = 0; i < values.GetSize(); ++i)
    {
        const math::Vector4& word = values[i];
        const StaticArray<float32, COLOR_CHANNELS> channels{{word.X, word.Y, word.Z, word.W}};
        for (usize channel = 0; channel < COLOR_CHANNELS; ++channel)
            std::memcpy(&bits[i * COLOR_CHANNELS + channel], &channels[channel], sizeof(uint32));
    }
    WriteIntegers(directory, scene, ".linear.bin", bits);
}
void CompareReference(rhi::DeviceHandle device,
                      rhi::SurfaceHandle surface,
                      SceneKind scene,
                      RenderPath path,
                      const rr::ReferenceCaptureDescription& description,
                      const Array<math::Vector4>& linear)
{
    VisibilityFixture fixture;
    PrepareFixture(device, path, scene, fixture);
    rr::ReferenceCapture capture;
    REQUIRE(IsAccepted(fixture.Renderer.BeginReferenceCapture(fixture.Snapshot, description, capture)) == true);
    Status status = Status::Pending;
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
        status = fixture.Renderer.AdvanceReferenceCapture(capture, surface);
    REQUIRE(status == Status::Ready);
    const usize pixelCount = usize(description.Width) * description.Height;
    Array<rr::ReferencePixel> reference;
    REQUIRE(reference.TryResize(pixelCount) == true);
    REQUIRE(fixture.Renderer.CopyReferencePixels(capture, reference.GetData(), pixelCount) == Status::Ready);
    rr::ReferenceCaptureInfo information;
    REQUIRE(fixture.Renderer.GetReferenceCaptureInfo(capture, information) == Status::Ready);
    CHECK(information.InvalidContributions == 0);
    if (scene == SceneKind::Empty)
        CHECK(information.VisibilityQueries == 0);
    else if (scene == SceneKind::Opaque || scene == SceneKind::Masked || scene == SceneKind::Mixed)
        CHECK(information.VisibilityQueries > pixelCount * description.TargetSamples / 2);
    else
        CHECK(information.VisibilityQueries > 0);
    for (usize pixel = 0; pixel < pixelCount; ++pixel)
    {
        CAPTURE(path, pixel);
        CHECK(reference[pixel].Samples == description.TargetSamples);
        const math::Vector4& value = linear[pixel * ACCUMULATION_WORDS];
        const math::Vector3 expected = reference[pixel].Radiance;
        CHECK(math::Abs(value.X - expected.X) <=
              LINEAR_ABSOLUTE_TOLERANCE + LINEAR_RELATIVE_TOLERANCE * math::Abs(expected.X));
        CHECK(math::Abs(value.Y - expected.Y) <=
              LINEAR_ABSOLUTE_TOLERANCE + LINEAR_RELATIVE_TOLERANCE * math::Abs(expected.Y));
        CHECK(math::Abs(value.Z - expected.Z) <=
              LINEAR_ABSOLUTE_TOLERANCE + LINEAR_RELATIVE_TOLERANCE * math::Abs(expected.Z));
    }
    REQUIRE(fixture.Renderer.Release(capture) == Status::Ready);
}
void RunPairedPreview(BenchmarkExperiment experiment)
{
    const bool shouldCacheHit = experiment == BenchmarkExperiment::HitCache;
    const bool shouldMeasurePasses = experiment == BenchmarkExperiment::PassTimings;
    const char* directoryOrNull = std::getenv(shouldMeasurePasses == true ? "LUDUS_RAY_PASS_BENCHMARK_DIRECTORY"
                                              : shouldCacheHit == true    ? "LUDUS_HIT_CACHE_BENCHMARK_DIRECTORY"
                                                                          : "LUDUS_VISIBILITY_BENCHMARK_DIRECTORY");
    const bool isBenchmark = directoryOrNull != nullptr;
    const bool shouldReverseOrder = std::getenv(shouldMeasurePasses == true ? "LUDUS_RAY_PASS_REVERSE_ORDER"
                                                : shouldCacheHit == true ? "LUDUS_HIT_CACHE_REVERSE_ORDER"
                                                                         : "LUDUS_VISIBILITY_REVERSE_ORDER") != nullptr;
    rr::internal::SetGpuPreviewOpacityPartitioningForTesting(true);
    const uint32 extent = isBenchmark == true ? BENCHMARK_EXTENT : TEST_EXTENT;
    const uint32 samples = isBenchmark == true ? BENCHMARK_SAMPLES : TEST_SAMPLES;
    for (usize sceneIndex = 0;
         sceneIndex < ((shouldCacheHit == true || shouldMeasurePasses == true) ? ALL_SCENE_COUNT : SCENE_COUNT);
         ++sceneIndex)
    {
        CAPTURE(sceneIndex);
        const SceneKind scene = static_cast<SceneKind>(sceneIndex);
        rhi::Shutdown();
        struct Guard final
        {
            LUDUS_INLINE ~Guard() noexcept
            {
                rhi::Shutdown();
            }
        } guard;
        rhi::DeviceDescription deviceDescription;
        deviceDescription.Required.PortableRaster = true;
        deviceDescription.Required.Compute = true;
        deviceDescription.Preferred.RayQueries = true;
        deviceDescription.EnableGpuTiming = isBenchmark == true || shouldMeasurePasses == true;
        rhi::DeviceHandle device;
        rhi::SurfaceHandle surface;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = extent, .Height = extent }, deviceDescription, device, surface);
        if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
            SKIP("No native GPU adapter");
        REQUIRE(started == rhi::DeviceStatus::Ready);
        rhi::RayCapabilities capabilities;
        if (rhi::GetRayCapabilities(device, capabilities) == Status::Unsupported)
            SKIP("Native ray queries unavailable");
        StaticArray<VisibilityFixture, VARIANT_COUNT> fixtures{};
        PrepareFixture(device,
                       shouldCacheHit == true        ? RenderPath::UncachedHit
                       : shouldMeasurePasses == true ? RenderPath::VisibilityOnly
                                                     : RenderPath::FullHit,
                       scene,
                       fixtures[0]);
        PrepareFixture(device,
                       shouldCacheHit == true ? RenderPath::CachedHit : RenderPath::VisibilityOnly,
                       scene,
                       fixtures[1]);
        const rr::DirectLight light
        {
            .SourceId = 1,
            .Intensity = 1,
            .Direction = {LIGHT_DIRECTION_X, 0, LIGHT_DIRECTION_Z},
        };
        rr::ReferenceCaptureDescription description;
        description.Width = description.Height = extent;
        description.TargetSamples = samples;
        description.MaxDepth = 2;
        description.RouletteDepth = 2;
        description.Seed = 1;
        description.OrthographicCamera = true;
        description.Environment = {ENVIRONMENT_RED, .5F, 1};
        description.Lights = &light;
        description.LightCount = 1;
        for (VisibilityFixture& fixture : fixtures)
        {
            REQUIRE(IsAccepted(fixture.Renderer.BeginGpuPreview(fixture.Snapshot, description, fixture.Preview)) ==
                    true);
            WaitSamples(fixture, 0);
        }
        TimingSamples timings;
        // Both immutable pipelines/scenes are ready before any timed frame. AB,BA
        // alternation pairs the same sample address, on one device/queue/surface.
        AdvancePairs(device, surface, fixtures, samples, isBenchmark, shouldMeasurePasses, shouldReverseOrder, timings);
        StaticArray<Array<uint8>, VARIANT_COUNT> pixels{};
        StaticArray<Array<math::Vector4>, VARIANT_COUNT> linear{};
        for (usize variant = 0; variant < VARIANT_COUNT; ++variant)
            ReadOutput(device, fixtures[variant], samples, pixels[variant], linear[variant]);
        REQUIRE(pixels[0].GetSize() == pixels[1].GetSize());
        CHECK(std::memcmp(pixels[0].GetData(), pixels[1].GetData(), pixels[0].GetSize()) == 0);
        REQUIRE(linear[0].GetSize() == linear[1].GetSize());
        for (usize pixel = 0; pixel < usize(extent) * extent; ++pixel)
        {
            const math::Vector4& first = linear[0][pixel * ACCUMULATION_WORDS];
            const math::Vector4& second = linear[1][pixel * ACCUMULATION_WORDS];
            CHECK(math::Abs(first.X - second.X) <= LINEAR_ABSOLUTE_TOLERANCE);
            CHECK(math::Abs(first.Y - second.Y) <= LINEAR_ABSOLUTE_TOLERANCE);
            CHECK(math::Abs(first.Z - second.Z) <= LINEAR_ABSOLUTE_TOLERANCE);
        }
        if (isBenchmark == true)
        {
            const std::string directory = directoryOrNull;
            const char* sceneName = SCENE_NAMES[sceneIndex];
            for (usize variant = 0; variant < VARIANT_COUNT; ++variant)
            {
                const std::string variantDirectory = directory + "/" +
                                                     (shouldMeasurePasses == true ? PASS_VARIANT_NAMES[variant]
                                                      : shouldCacheHit == true    ? CACHE_VARIANT_NAMES[variant]
                                                                                  : VARIANT_NAMES[variant]);
                WriteArtifact(variantDirectory,
                              sceneName,
                              ".rgba",
                              pixels[variant].GetData(),
                              pixels[variant].GetSize());
                WriteLinear(variantDirectory, sceneName, linear[variant]);
                WriteTimings(variantDirectory, sceneName, timings.GpuNanoseconds[variant]);
                WriteIntegers(variantDirectory, sceneName, ".wall.bin", timings.WallNanoseconds[variant]);
                if (shouldMeasurePasses == true && variant == PROFILED_VARIANT_INDEX)
                    WritePassTimings(variantDirectory, sceneName, timings.Passes);
            }
            WriteIntegers(directory, sceneName, ".gap.bin", timings.PairGapNanoseconds);
            WriteIntegers(directory, sceneName, ".order.bin", timings.IsBaselineFirst);
            const StaticArray<uint32, CONFIGURATION_WORD_COUNT> configuration{
                {BENCHMARK_FORMAT_VERSION, extent, samples, WARMUP_SAMPLES}};
            StaticArray<uint8, CONFIGURATION_WORD_COUNT * sizeof(uint32)> bytes{};
            for (usize i = 0; i < CONFIGURATION_WORD_COUNT; ++i)
                REQUIRE(TryWriteLittleEndian(configuration[i],
                                             {bytes.GetData() + i * sizeof(uint32), sizeof(uint32)}) == true);
            WriteArtifact(directory, sceneName, ".configuration.bin", bytes.GetData(), bytes.GetSize());
        }
        else
        {
            CompareReference(device, surface, scene, RenderPath::CpuOracle, description, linear[1]);
            CompareReference(device, surface, scene, RenderPath::GpuReference, description, linear[1]);
        }
        for (VisibilityFixture& fixture : fixtures)
            REQUIRE(fixture.Renderer.Release(fixture.Preview) == Status::Ready);
        const uint64 start = time::NowTicks();
        rhi::RayResourceStats resources;
        do
        {
            REQUIRE(rhi::PollLifetime(device) == Status::Ready);
            for (VisibilityFixture& fixture : fixtures)
                REQUIRE(fixture.Renderer.PollGpuPreview() == Status::Ready);
            REQUIRE(rhi::GetRayResourceStats(device, resources) == Status::Ready);
        } while ((resources.Geometries != 0 || resources.Scenes != 0) && time::NowTicks() - start < TIMEOUT);
        CHECK(resources.Geometries == 0);
        CHECK(resources.Scenes == 0);
    }
}
} // namespace

TEST_CASE("Visibility-only queries preserve full-hit output and CPU/GPU reference parity",
          "[renderer][gpu][visibility]")
{
    RunPairedPreview(BenchmarkExperiment::Visibility);
}
TEST_CASE("Per-hit material and UV caching preserves uncached output and CPU/GPU reference parity",
          "[renderer][gpu][hit-cache]")
{
    RunPairedPreview(BenchmarkExperiment::HitCache);
}

TEST_CASE("Per-pass GPU probes preserve ray rendering and separate transport from output",
          "[renderer][gpu][ray-pass-timings]")
{
    RunPairedPreview(BenchmarkExperiment::PassTimings);
}
