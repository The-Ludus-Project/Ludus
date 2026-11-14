#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>

/// @file
/// Device-side raster pipeline preparation counters (issue #1253).
///
/// The offline graph build reports portable recipe keys and artifact bytes; this
/// report covers what the device does with them: native pipeline creations,
/// requests that reuse an existing native pipeline, preparation time, pipelines
/// a draw needed while they were still preparing, and the pipelines resident on
/// the device. Timing is owner-thread wall time from the process monotonic clock;
/// it is diagnostic and never enters pipeline, recipe or package identity.
/// Compute pipelines are not counted. See
/// docs/development/render-graph-authoring.md, "Device pipeline preparation".
namespace ludus::graphics::rhi
{
/// Preparation history of one native raster pipeline on its device.
struct PipelinePreparation final
{
    /// Owned RequestPipeline requests currently sharing this native pipeline; zero for direct creation.
    ludus::foundation::uint32 Requests = 0;
    /// True for a request query when the request joined a native pipeline that an earlier request had
    /// already queued or created, so it launched no native creation. Always false for a handle query.
    bool IsShared = false;
    /// True once native creation launched on the device owner thread.
    bool IsLaunched = false;
    /// True once native creation completed with Ready; PreparationSeconds is then final.
    bool IsReady = false;
    /// True when native creation completed with a failure.
    bool HasFailed = false;
    /// Seconds from the request that queued native creation to its launch by PollLifetime.
    /// Zero for direct CreateRasterPipeline creation, which launches immediately.
    ludus::foundation::float64 QueueSeconds = 0;
    /// Seconds from native creation launch until the device owner thread observed its outcome. A synchronous
    /// backend measures the creation call; asynchronous preparation also includes worker queueing and the delay
    /// until the owner thread polls its completion. Zero until the outcome is observed.
    ludus::foundation::float64 PreparationSeconds = 0;
    /// True when a draw-time use found this pipeline still preparing: a DrawIndexed or RecordDraw packet
    /// referencing it, or GetRequestedPipeline inside an open frame. Such a draw is rejected or skipped; the
    /// RHI never creates a native pipeline inside a frame.
    bool HasFirstUseMiss = false;
    /// True after a draw packet referencing this pipeline passed validation.
    bool HasBeenDrawn = false;
};
/// Query one direct or borrowed raster pipeline's preparation history.
/// @param device Ready device that owns the pipeline.
/// @param pipeline Pipeline from CreateRasterPipeline, or borrowed through GetRequestedPipeline.
/// @param outPreparation Written on Ready; preserved on failure.
/// @return Ready, InvalidHandle for stale/foreign/destroyed handles, or the device admission status.
/// Owner thread only; no allocation, GPU work or completion polling.
[[nodiscard]] RasterStatus GetPipelinePreparation(DeviceHandle device,
                                                  RasterPipelineHandle pipeline,
                                                  PipelinePreparation& outPreparation) noexcept;
/// Query the preparation history of the native pipeline that an owned request shares.
/// @param device Ready device that owns the request.
/// @param request Owned, unreleased request from RequestPipeline.
/// @param outPreparation Written on Ready, with IsShared describing this request; preserved on failure.
/// @return Ready, InvalidHandle for stale/foreign/released requests, or the device admission status.
/// Owner thread only; no allocation, GPU work or completion polling.
[[nodiscard]] RasterStatus
GetPipelinePreparation(DeviceHandle device, PipelineRequest request, PipelinePreparation& outPreparation) noexcept;

/// Raster pipeline preparation totals of one device session, reset when the device is destroyed.
struct PipelinePreparationReport final
{
    /// Backend that prepared the pipelines; Vulkan until the device is Ready.
    Backend SelectedBackend = Backend::Vulkan;
    /// Native raster pipelines whose creation completed and that the device still holds, including
    /// pipelines retained by recorded or in-flight work after their public handle or request was released.
    ludus::foundation::uint32 ResidentPipelines = 0;
    /// Native raster pipelines queued for creation or still preparing.
    ludus::foundation::uint32 PreparingPipelines = 0;
    /// Native raster pipeline creations launched by this session, including failed ones.
    ludus::foundation::uint64 NativeCreations = 0;
    /// RequestPipeline calls that joined an existing native pipeline instead of creating one.
    ludus::foundation::uint64 SharedRequests = 0;
    /// Pipelines whose draw-time use found them still preparing; each pipeline is counted once.
    ludus::foundation::uint64 FirstUseMisses = 0;
    /// Native creations that completed with a failure.
    ludus::foundation::uint64 FailedPreparations = 0;
    /// Sum of PipelinePreparation::PreparationSeconds over completed creations. Concurrent asynchronous
    /// preparations overlap, so the sum can exceed the elapsed wall time.
    ludus::foundation::float64 PreparationSeconds = 0;
    /// True only when the backend can measure driver memory held by pipelines. No current backend can:
    /// Metal, Vulkan, Direct3D 12, WebGPU and WebGL 2 expose no per-pipeline allocation size. Shader
    /// artifact or package bytes are not a substitute for this value.
    bool HasDriverMemory = false;
    /// Driver bytes held by resident pipelines; zero unless HasDriverMemory.
    ludus::foundation::uint64 DriverMemoryBytes = 0;
};
/// Query raster pipeline preparation totals for the device session.
/// @param device Ready device.
/// @param outReport Written on Ready; preserved on failure.
/// @return Ready or the device admission status. Owner thread only; no allocation or GPU work.
[[nodiscard]] RasterStatus GetPipelinePreparationReport(DeviceHandle device,
                                                        PipelinePreparationReport& outReport) noexcept;
} // namespace ludus::graphics::rhi
