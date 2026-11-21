#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "mip_streaming.h"

#include <cinttypes>
#include <cstdio>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
using rhi::RasterStatus;

constexpr uint32 TEXTURE_EXTENT = 8;
constexpr uint32 COARSE_MIP = 2;
constexpr uint32 FINE_MIP = 1;
constexpr uint32 ALLOCATED_MIPS = COARSE_MIP + 1;
constexpr uint32 TARGET_EXTENT = 4;
constexpr uint32 RGBA_CHANNELS = 4;
constexpr uint32 CAMERA_COMPONENTS = 4;
constexpr uint8 FULL_CHANNEL = 255;
constexpr usize TRANSFER_BUDGET_BYTES = 1024;
constexpr uint64 RESOURCE_BUDGET_BYTES = 2048;
constexpr uint32 OVER_BUDGET_TEXTURE_EXTENT = 32;
constexpr uint64 WAIT_NANOSECONDS = time::Seconds(5).Nanoseconds;
constexpr uint32 COARSE_MASK = 1U << COARSE_MIP;
constexpr uint32 REFINED_MASK = COARSE_MASK | (1U << FINE_MIP);
constexpr uint32 INDEX_COUNT = 6;
constexpr uint32 VERTEX_COUNT = 4;

struct PlaneVertex
{
    float32 PositionMesh[2];
    float32 Uv[2];
};

struct Probe
{
    float32 Scale;
    float32 OriginalLod;
    uint8 RefinedRed;
    uint8 RefinedGreen;
};

// Thanks to Lance Williams, Pyramidal Parametrics (SIGGRAPH '83), "Parametric
// Interpolation", for the between-level interpolation tested here. This fixture
// uses explicitly initialized constant colors to isolate mip selection from UV filtering.
// https://www.cs.cmu.edu/afs/cs/academic/class/15869-f11/www/readings/williams83_mipmap.pdf
// The same fixed dolly positions and original texture LODs are used for
// coarse, pending and refined draws. LOD is log2(allocation width / projected
// width), with clip scale rounded for the tiny fixture. Probe zero straddles
// both resident mips.
constexpr Probe PROBES[]{
    {0.84F, 1.25F, 64, 191},
    {0.71F, 1.5F, 128, 128},
    {0.42F, 2.25F, FULL_CHANNEL, 0},
};
constexpr usize PROBE_COUNT = sizeof(PROBES) / sizeof(Probe);

struct Binding
{
    rhi::TextureHandle Texture;
    rhi::TextureViewHandle View;
    rhi::BufferHandle Cameras[PROBE_COUNT];
    rhi::BindingSetHandle Sets[PROBE_COUNT];
    uint32 BaseMip = 0;
    uint32 MipCount = 0;
};

template <typename Value, usize Count>
BorrowedView<const uint8> GetBytes(const Value (&values)[Count]) noexcept
{
    return {reinterpret_cast<const uint8*>(values), sizeof(values)};
}

bool Check(RasterStatus status, RasterStatus expected, const char* operation) noexcept
{
    if (status == expected)
    {
        return true;
    }
    std::fprintf(stderr,
                 "%s returned %u, expected %u\n",
                 operation,
                 static_cast<unsigned>(status),
                 static_cast<unsigned>(expected));
    return false;
}

class StreamingFixture final
{
public:
    bool Initialize() noexcept;
    bool Run(const char* workload, uint64 started) noexcept;

private:
    template <typename Ticket>
    bool wait(Ticket ticket, const char* operation) const noexcept
    {
        const uint64 started = time::NowTicks();
        RasterStatus status = rhi::GetStatus(mDevice, ticket);
        while (status == RasterStatus::Pending && time::NowTicks() - started < WAIT_NANOSECONDS)
        {
            threading::YieldThread();
            status = rhi::GetStatus(mDevice, ticket);
        }
        return Check(status, RasterStatus::Ready, operation);
    }

    bool allocate(Binding& outBinding) const noexcept;
    template <uint32 Mip>
    bool requestMip(rhi::TextureHandle texture, RasterStatus expected, rhi::TransferTicket& outTicket) const noexcept;
    bool finishMip(rhi::TransferTicket& inoutTicket) const noexcept;
    bool checkResidency(rhi::TextureHandle texture, uint32 mask, uint64 bytes) const noexcept;
    bool bind(Binding& inoutBinding, uint32 baseMip, uint32 mipCount) const noexcept;
    bool retire(Binding& inoutBinding) const noexcept;
    bool submit(const Binding& binding,
                usize index,
                rhi::TextureHandle target,
                rhi::SubmissionToken& outCompletion) const noexcept;
    bool verify(rhi::SubmissionToken completion,
                rhi::TextureHandle target,
                const Binding& binding,
                usize index,
                const char* phase) const noexcept;
    bool sweep(const Binding& binding, const char* phase) const noexcept;
    void trace(const char* event, uint32 mask = 0, uint64 payloadBytes = 0) const noexcept;

    rhi::DeviceHandle mDevice;
    rhi::SurfaceHandle mSurface;
    rhi::TextureHandle mTarget;
    rhi::TextureHandle mRetainedTarget;
    rhi::BufferHandle mVertices;
    rhi::BufferHandle mIndices;
    rhi::SamplerHandle mSampler;
    rhi::BindingLayoutHandle mLayout;
    rhi::RasterPipelineHandle mPipeline;
    const char* mWorkload = "setup";
    uint64 mStarted = 0;
};

bool StreamingFixture::Initialize() noexcept
{
    rhi::DeviceDescription device;
    device.Required.PortableRaster = true;
    device.RequiredProfile.FormatCount = 1;
    rhi::FormatRequirement& format = device.RequiredProfile.Formats[0];
    format.Uses.Sampled = true;
    format.Uses.Filterable = true;
    format.Uses.CopySource = true;
    format.Uses.CopyDestination = true;
    format.PartialViews = true;
    format.MinMipLevels = ALLOCATED_MIPS;
    device.Storage.TransferSlices = 2;
    device.Storage.TransferBytes = TRANSFER_BUDGET_BYTES;
    device.Storage.ResourceBytes = RESOURCE_BUDGET_BYTES;
    rhi::StartupInfo startup;
    const rhi::DeviceStatus created =
        rhi::CreateDevice({}, { .Width = TARGET_EXTENT, .Height = TARGET_EXTENT }, device, mDevice, mSurface, &startup);
    if (created != rhi::DeviceStatus::Ready)
    {
        std::fprintf(stderr,
                     "Native streaming device creation returned %u (error=%u, profile=%u)\n",
                     static_cast<unsigned>(created),
                     static_cast<unsigned>(startup.Error),
                     static_cast<unsigned>(startup.ProfileFailure.Requirement));
        return false;
    }

    constexpr PlaneVertex VERTICES[VERTEX_COUNT]{
        {{-1, 1}, {0, 0}},
        {{-1, -1}, {0, 1}},
        {{1, 1}, {1, 0}},
        {{1, -1}, {1, 1}},
    };
    constexpr uint16 INDICES[INDEX_COUNT]{0, 1, 2, 2, 1, 3};
    constexpr rhi::TextureDescription TARGET{ .Width = TARGET_EXTENT, .Height = TARGET_EXTENT, .Attachment = true };
    const rhi::RasterShaderDescription vertexCode = ludus::shaders::mip_streaming::Vertex();
    const rhi::RasterShaderDescription fragmentCode = ludus::shaders::mip_streaming::Fragment();
    rhi::RasterShaderHandle vertex;
    rhi::RasterShaderHandle fragment;
    // CameraSettings is referenced by both stages. Use the fixture's union layout.
    constexpr rhi::RasterBinding BINDINGS[]{
        {
            .Binding = 0,
            .Kind = rhi::RasterBindingKind::UniformBuffer,
            .Visibility = rhi::RasterVisibility::Both,
            .MinSize = sizeof(float32) * CAMERA_COMPONENTS,
        },
        { .Binding = 1, .Kind = rhi::RasterBindingKind::Texture2D, .Visibility = rhi::RasterVisibility::Fragment },
        { .Binding = 2, .Kind = rhi::RasterBindingKind::Sampler, .Visibility = rhi::RasterVisibility::Fragment },
    };
    if (Check(rhi::CreateBuffer(mDevice, {rhi::BufferRole::Vertex, sizeof(VERTICES)}, GetBytes(VERTICES), mVertices),
              RasterStatus::Ready,
              "create plane vertices") == false ||
        Check(rhi::CreateBuffer(mDevice, {rhi::BufferRole::Index16, sizeof(INDICES)}, GetBytes(INDICES), mIndices),
              RasterStatus::Ready,
              "create plane indices") == false ||
        Check(rhi::CreateTexture(mDevice, TARGET, {}, mTarget), RasterStatus::Ready, "create sweep target") == false ||
        Check(rhi::CreateTexture(mDevice, TARGET, {}, mRetainedTarget),
              RasterStatus::Ready,
              "create retained target") == false ||
        Check(rhi::CreateSampler(mDevice,
                                 { .Filter = rhi::RasterFilter::Linear, .MipFilter = rhi::RasterMipFilter::Linear },
                                 mSampler),
              RasterStatus::Ready,
              "create trilinear sampler") == false ||
        Check(rhi::CreateRasterShader(mDevice, vertexCode, vertex),
              RasterStatus::Ready,
              "create streaming vertex shader") == false ||
        Check(rhi::CreateRasterShader(mDevice, fragmentCode, fragment),
              RasterStatus::Ready,
              "create streaming fragment shader") == false ||
        Check(rhi::CreateBindingLayout(mDevice, BINDINGS, mLayout), RasterStatus::Ready, "create streaming layout") ==
            false)
    {
        return false;
    }

    constexpr uint32 UV_OFFSET = sizeof(PlaneVertex::PositionMesh);
    constexpr rhi::RasterVertexStream STREAMS[]{{sizeof(PlaneVertex), false}};
    constexpr rhi::RasterVertexAttribute ATTRIBUTES[]{
        {0, 0, 0, rhi::RasterVertexFormat::Float2},
        {1, 0, UV_OFFSET, rhi::RasterVertexFormat::Float2},
    };
    return Check(rhi::CreateRasterPipeline(mDevice,
                                           {
                                               .Vertex = vertex,
                                               .Fragment = fragment,
                                               .Layout = mLayout,
                                               .Streams = STREAMS,
                                               .Attributes = ATTRIBUTES,
                                               .Target = rhi::RasterTarget::Rgba8Unorm,
                                           },
                                           mPipeline),
                 RasterStatus::Ready,
                 "create streaming pipeline");
}

bool StreamingFixture::allocate(Binding& outBinding) const noexcept
{
    return Check(rhi::CreateTexture(mDevice,
                                    {
                                        .Width = TEXTURE_EXTENT,
                                        .Height = TEXTURE_EXTENT,
                                        .MipLevels = ALLOCATED_MIPS,
                                        .DeferredInitialization = true,
                                    },
                                    {},
                                    outBinding.Texture),
                 RasterStatus::Ready,
                 "allocate deferred texture");
}

template <uint32 Mip>
bool StreamingFixture::requestMip(rhi::TextureHandle texture,
                                  RasterStatus expected,
                                  rhi::TransferTicket& outTicket) const noexcept
{
    constexpr uint32 EXTENT = TEXTURE_EXTENT >> Mip;
    constexpr usize ROW_BYTES = EXTENT * RGBA_CHANNELS;
    uint8 pixels[EXTENT * ROW_BYTES]{};
    for (usize offset = 0; offset < sizeof(pixels); offset += RGBA_CHANNELS)
    {
        pixels[offset] = Mip == COARSE_MIP ? FULL_CHANNEL : 0;
        pixels[offset + 1] = Mip == FINE_MIP ? FULL_CHANNEL : 0;
        pixels[offset + RGBA_CHANNELS - 1] = FULL_CHANNEL;
    }
    return Check(rhi::RequestTextureUpdate(mDevice,
                                           texture,
                                           {Mip, 0, 0, EXTENT, EXTENT},
                                           pixels,
                                           sizeof(pixels),
                                           ROW_BYTES,
                                           outTicket),
                 expected,
                 "request mip transfer");
}

bool StreamingFixture::finishMip(rhi::TransferTicket& inoutTicket) const noexcept
{
    return wait(inoutTicket, "complete mip transfer") == true &&
           Check(rhi::Release(mDevice, inoutTicket), RasterStatus::Ready, "release completed mip transfer");
}

bool StreamingFixture::checkResidency(rhi::TextureHandle texture, uint32 mask, uint64 bytes) const noexcept
{
    rhi::TextureResidency residency;
    if (Check(rhi::GetTextureResidency(mDevice, texture, residency), RasterStatus::Ready, "read mip residency") ==
        false)
    {
        return false;
    }
    if (residency.MipLevels == ALLOCATED_MIPS && residency.InitializedMips == mask &&
        residency.InitializedBytes == bytes)
    {
        return true;
    }
    std::fprintf(stderr,
                 "Residency mismatch, mask=%u bytes=%" PRIu64 " levels=%u\n",
                 residency.InitializedMips,
                 residency.InitializedBytes,
                 residency.MipLevels);
    return false;
}

bool StreamingFixture::bind(Binding& inoutBinding, uint32 baseMip, uint32 mipCount) const noexcept
{
    if (Check(rhi::CreateTextureView(mDevice,
                                     inoutBinding.Texture,
                                     { .BaseMip = baseMip, .MipCount = mipCount },
                                     inoutBinding.View),
              RasterStatus::Ready,
              "create resident mip view") == false)
    {
        return false;
    }
    inoutBinding.BaseMip = baseMip;
    inoutBinding.MipCount = mipCount;
    for (usize index = 0; index < PROBE_COUNT; ++index)
    {
        const float32 camera[CAMERA_COMPONENTS]{PROBES[index].Scale,
                                                PROBES[index].OriginalLod,
                                                static_cast<float32>(baseMip),
                                                0};
        if (Check(rhi::CreateBuffer(mDevice,
                                    {rhi::BufferRole::Uniform, sizeof(camera)},
                                    GetBytes(camera),
                                    inoutBinding.Cameras[index]),
                  RasterStatus::Ready,
                  "create camera buffer") == false)
        {
            return false;
        }
        const rhi::RasterBindingResource resources[]{
            { .Buffer = inoutBinding.Cameras[index], .Size = sizeof(camera) },
            { .Binding = 1, .Texture = inoutBinding.View },
            { .Binding = 2, .Sampler = mSampler },
        };
        if (Check(rhi::CreateBindingSet(mDevice, mLayout, resources, inoutBinding.Sets[index]),
                  RasterStatus::Ready,
                  "create immutable mip binding") == false)
        {
            return false;
        }
    }
    return true;
}

bool StreamingFixture::retire(Binding& inoutBinding) const noexcept
{
    for (usize index = 0; index < PROBE_COUNT; ++index)
    {
        if (Check(rhi::Destroy(mDevice, inoutBinding.Cameras[index]), RasterStatus::Ready, "retire camera buffer") ==
                false ||
            Check(rhi::Destroy(mDevice, inoutBinding.Sets[index]), RasterStatus::Ready, "retire mip binding") == false)
        {
            return false;
        }
    }
    return true;
}

bool StreamingFixture::submit(const Binding& binding,
                              usize index,
                              rhi::TextureHandle target,
                              rhi::SubmissionToken& outCompletion) const noexcept
{
    if (rhi::BeginFrame(mDevice, mSurface) != rhi::DeviceStatus::Ready)
    {
        std::fputs("Could not begin native streaming frame\n", stderr);
        return false;
    }
    constexpr usize VERTEX_BYTES = VERTEX_COUNT * sizeof(PlaneVertex);
    const rhi::RasterVertexSlice slices[]{{mVertices, 0, VERTEX_BYTES}};
    const rhi::RasterDraw draw{mPipeline, binding.Sets[index], slices, mIndices, 0, INDEX_COUNT, VERTEX_COUNT};
    return Check(rhi::BeginRasterPass(mDevice,
                                      {
                                          .Color = target,
                                      }),
                 RasterStatus::Ready,
                 "begin streamed raster pass") == true &&
           Check(rhi::DrawIndexed(mDevice, draw), RasterStatus::Ready, "draw streamed plane") == true &&
           Check(rhi::EndFrame(mDevice, outCompletion), RasterStatus::Ready, "submit streamed frame");
}

bool StreamingFixture::verify(rhi::SubmissionToken completion,
                              rhi::TextureHandle target,
                              const Binding& binding,
                              usize index,
                              const char* phase) const noexcept
{
    uint8 pixel[RGBA_CHANNELS]{};
    rhi::ReadbackTicket readback;
    constexpr rhi::TextureRegion CENTER_PIXEL{0, TARGET_EXTENT / 2, TARGET_EXTENT / 2, 1, 1};
    if (Check(rhi::RequestTextureReadback(mDevice, target, CENTER_PIXEL, RGBA_CHANNELS, readback),
              RasterStatus::Pending,
              "read streamed pixel") == false ||
        wait(completion, "complete streamed frame") == false ||
        wait(readback, "complete streamed pixel readback") == false ||
        Check(rhi::CopyReadback(mDevice, readback, pixel, sizeof(pixel)), RasterStatus::Ready, "copy pixel") == false ||
        Check(rhi::Release(mDevice, readback), RasterStatus::Ready, "release pixel readback") == false)
    {
        return false;
    }
    const bool hasFine = binding.BaseMip == FINE_MIP;
    const uint8 expectedRed = hasFine == true ? PROBES[index].RefinedRed : FULL_CHANNEL;
    const uint8 expectedGreen = hasFine == true ? PROBES[index].RefinedGreen : 0;
    const int32 redError = static_cast<int32>(pixel[0]) - expectedRed;
    const int32 greenError = static_cast<int32>(pixel[1]) - expectedGreen;
    std::printf("%s,%s,%" PRIu64 ",lod=%.2f,base=%u,count=%u,rgba=%u/%u/%u/%u\n",
                mWorkload,
                phase,
                time::NowTicks() - mStarted,
                static_cast<double>(PROBES[index].OriginalLod),
                binding.BaseMip,
                binding.MipCount,
                unsigned{pixel[0]},
                unsigned{pixel[1]},
                unsigned{pixel[2]},
                unsigned{pixel[RGBA_CHANNELS - 1]});
    // Round-to-nearest UNORM8 ties and native filtering precision can differ by one code.
    const int32 tolerance = hasFine == true && expectedGreen != 0 ? 1 : 0;
    if (redError < -tolerance || redError > tolerance || greenError < -tolerance || greenError > tolerance ||
        pixel[2] != binding.MipCount || pixel[RGBA_CHANNELS - 1] != FULL_CHANNEL)
    {
        std::fprintf(stderr,
                     "Unexpected scene-linear sample at %s probe %zu (expected R=%u G=%u)\n",
                     phase,
                     index,
                     unsigned{expectedRed},
                     unsigned{expectedGreen});
        return false;
    }
    return true;
}

bool StreamingFixture::sweep(const Binding& binding, const char* phase) const noexcept
{
    for (usize index = 0; index < PROBE_COUNT; ++index)
    {
        rhi::SubmissionToken completion;
        if (submit(binding, index, mTarget, completion) == false ||
            verify(completion, mTarget, binding, index, phase) == false)
        {
            return false;
        }
    }
    return true;
}

void StreamingFixture::trace(const char* event, uint32 mask, uint64 payloadBytes) const noexcept
{
    rhi::StorageUsage usage;
    if (rhi::GetStorageUsage(mDevice, usage) == RasterStatus::Ready)
    {
        std::printf("%s,%s,%" PRIu64 ",mask=%u,payload_bytes=%" PRIu64 ",logical_resource_bytes=%" PRIu64
                    ",detached_records=%" PRIu64 "\n",
                    mWorkload,
                    event,
                    time::NowTicks() - mStarted,
                    mask,
                    payloadBytes,
                    usage.ResourceBytes,
                    usage.DetachedRecords);
    }
}

bool StreamingFixture::Run(const char* workload, uint64 started) noexcept
{
    constexpr uint64 COARSE_BYTES = (TEXTURE_EXTENT >> COARSE_MIP) * (TEXTURE_EXTENT >> COARSE_MIP) * RGBA_CHANNELS;
    constexpr uint64 FINE_BYTES = (TEXTURE_EXTENT >> FINE_MIP) * (TEXTURE_EXTENT >> FINE_MIP) * RGBA_CHANNELS;
    mWorkload = workload;
    mStarted = started;
    rhi::StorageUsage baseline;
    if (Check(rhi::GetStorageUsage(mDevice, baseline), RasterStatus::Ready, "read baseline storage") == false)
    {
        return false;
    }
    trace("start");
    Binding coarse;
    Binding replacement;
    rhi::TransferTicket transfer;
    if (allocate(coarse) == false || requestMip<0>(coarse.Texture, RasterStatus::CapacityExceeded, transfer) == false ||
        checkResidency(coarse.Texture, 0, 0) == false)
    {
        return false;
    }
    trace("over_budget_mip_rejected");
    if (requestMip<COARSE_MIP>(coarse.Texture, RasterStatus::Pending, transfer) == false ||
        finishMip(transfer) == false || checkResidency(coarse.Texture, COARSE_MASK, COARSE_BYTES) == false ||
        bind(coarse, COARSE_MIP, 1) == false)
    {
        return false;
    }
    trace("coarse_published", COARSE_MASK, COARSE_BYTES);
    constexpr rhi::TextureDescription OVERSIZED
    {
        .Width = OVER_BUDGET_TEXTURE_EXTENT,
        .Height = OVER_BUDGET_TEXTURE_EXTENT,
        .DeferredInitialization = true,
    };
    rhi::TextureHandle rejected;
    if (Check(rhi::CreateTexture(mDevice, OVERSIZED, {}, rejected),
              RasterStatus::CapacityExceeded,
              "reject over-budget replacement allocation") == false ||
        rhi::IsNull(rejected) == false)
    {
        return false;
    }
    trace("over_budget_allocation_rejected", COARSE_MASK);
    if (sweep(coarse, "coarse_sample") == false || allocate(replacement) == false ||
        requestMip<COARSE_MIP>(replacement.Texture, RasterStatus::Pending, transfer) == false ||
        finishMip(transfer) == false)
    {
        return false;
    }

    // Cancel one generation after its real GPU transfer was admitted. Keep the
    // published binding while the obsolete allocation and upload retire.
    if (requestMip<FINE_MIP>(replacement.Texture, RasterStatus::Pending, transfer) == false)
    {
        return false;
    }
    const rhi::TransferTicket stale = transfer;
    const rhi::TextureHandle staleTexture = replacement.Texture;
    if (Check(rhi::Release(mDevice, transfer), RasterStatus::Ready, "cancel obsolete refinement") == false ||
        Check(rhi::Destroy(mDevice, replacement.Texture), RasterStatus::Ready, "retire obsolete texture") == false ||
        allocate(replacement) == false ||
        requestMip<COARSE_MIP>(replacement.Texture, RasterStatus::Pending, transfer) == false ||
        Check(rhi::GetStatus(mDevice, stale), RasterStatus::InvalidHandle, "reject stale ticket") == false ||
        Check(rhi::GetStatus(mDevice, staleTexture), RasterStatus::InvalidHandle, "reject obsolete texture") == false ||
        finishMip(transfer) == false)
    {
        return false;
    }
    trace("cancelled_generation_rejected", COARSE_MASK, FINE_BYTES);
    if (requestMip<FINE_MIP>(replacement.Texture, RasterStatus::Pending, transfer) == false ||
        checkResidency(replacement.Texture, COARSE_MASK, COARSE_BYTES) == false)
    {
        return false;
    }
    trace("fine_requested", COARSE_MASK, FINE_BYTES);
    if (sweep(coarse, "pending_publication_sample") == false || finishMip(transfer) == false ||
        checkResidency(replacement.Texture, REFINED_MASK, COARSE_BYTES + FINE_BYTES) == false)
    {
        return false;
    }

    trace("fine_ready", REFINED_MASK, FINE_BYTES);
    if (bind(replacement, FINE_MIP, 2) == false)
    {
        return false;
    }

    // The published sets retain the original view and backing allocation even
    // after the public texture and view handles have been destroyed.
    rhi::SubmissionToken retained;
    constexpr usize RETAINED_PROBE = 0;
    if (Check(rhi::Destroy(mDevice, coarse.Texture), RasterStatus::Ready, "detach old texture") == false ||
        Check(rhi::Destroy(mDevice, coarse.View), RasterStatus::Ready, "detach old view") == false ||
        submit(coarse, RETAINED_PROBE, mRetainedTarget, retained) == false)
    {
        return false;
    }
    if (retire(coarse) == false)
    {
        return false;
    }
    const RasterStatus oldFrame = rhi::GetStatus(mDevice, retained);
    if (oldFrame != RasterStatus::Ready && oldFrame != RasterStatus::Pending)
    {
        return Check(oldFrame, RasterStatus::Ready, "query previous frame after binding release");
    }
    rhi::StorageUsage afterRelease;
    if (Check(rhi::GetStorageUsage(mDevice, afterRelease), RasterStatus::Ready, "read in-flight storage") == false ||
        (oldFrame == RasterStatus::Pending && afterRelease.DetachedRecords <= baseline.DetachedRecords))
    {
        std::fputs("Pending frame did not retain detached resources\n", stderr);
        return false;
    }
    trace(oldFrame == RasterStatus::Pending ? "old_binding_released_in_flight" : "old_frame_completed_during_release");
    trace("refined_published", REFINED_MASK, COARSE_BYTES + FINE_BYTES);
    if (sweep(replacement, "refined_sample") == false ||
        verify(retained, mRetainedTarget, coarse, RETAINED_PROBE, "retained_old_sample") == false ||
        retire(replacement) == false ||
        Check(rhi::Destroy(mDevice, replacement.View), RasterStatus::Ready, "release refined view") == false ||
        Check(rhi::Destroy(mDevice, replacement.Texture), RasterStatus::Ready, "release refined texture") == false)
    {
        return false;
    }
    rhi::StorageUsage finalUsage;
    if (Check(rhi::GetStorageUsage(mDevice, finalUsage), RasterStatus::Ready, "read retired storage") == false ||
        finalUsage.ResourceBytes != baseline.ResourceBytes || finalUsage.DetachedRecords != baseline.DetachedRecords)
    {
        std::fputs("Streamed resource storage did not return to baseline\n", stderr);
        return false;
    }
    trace("complete");
    return true;
}
} // namespace

int main()
{
    const uint64 started = time::NowTicks();
    StreamingFixture fixture;
    std::puts("workload,event,elapsed_ns,measurements");
    const bool hasPassed = fixture.Initialize() == true && fixture.Run("cold", started) == true &&
                           fixture.Run("warm", time::NowTicks()) == true;
    rhi::Shutdown();
    return hasPassed == true ? 0 : 1;
}
