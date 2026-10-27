#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

// One diagnostic ray path capture of a frozen render-graph preview: a separate RenderGraph
// initialized with the `LUDUS_RAY_PATH_CAPTURE` variant of the frozen generation, fed the frozen
// extraction and camera, rendered until its samples complete and read back without waiting.
// The live graph's drawable scene and output are never involved. See "Ray path inspection" in
// docs/architecture/render-graph-authoring.md.
namespace ludus::editor
{
struct RenderGraphExtraction;

/// Lifecycle of one ray path capture, as the inspector reports it.
enum class RayPathCaptureState : foundation::uint8
{
    Idle,        ///< No capture was requested.
    Building,    ///< The capture variant of the frozen generation is being compiled.
    Preparing,   ///< The diagnostic graph's pipelines and its scene are preparing.
    Tracing,     ///< Samples are rendering from the frozen camera.
    Reading,     ///< The capture buffer is being read back and decoded.
    Ready,       ///< Events and summaries are decoded.
    Failed,      ///< Building, preparation, tracing or readback failed; the message says why.
    Unavailable, ///< This device, renderer selection or generation cannot capture ray paths.
};

/// The live graph's inputs copied when the preview was frozen for a capture.
struct RayPathFreeze final
{
    /// Graph camera (conventional-depth world-to-clip, position, projection) of the frozen frame.
    graphics::renderer::ViewDescription View{};
    /// Drawable reference size the graph resolved its relative resources against, in pixels.
    QSize ReferenceSize;
    /// Extent of the exported color result the author picks a pixel in, in pixels.
    QSize OutputSize;
    /// World extraction revision; the capture graph uses it, so it also seeds sampling.
    foundation::uint64 SceneRevision = 0;
    /// Non-camera content revision of the frozen extraction.
    foundation::uint64 ContentRevision = 0;
    /// Activation serial of the frozen generation.
    foundation::uint64 Generation = 0;
    /// RenderGraphPackage generation identity of the frozen runtime package.
    QByteArray GenerationIdentity;
    /// CPU interface identity of the frozen runtime package.
    QByteArray InterfaceIdentity;
    /// Runtime package the frozen generation was decoded from.
    QString PackagePath;
};

/// What a capture traces: one canvas node's ray pass, or every ray pass of the configuration.
struct RayPathTarget final
{
    /// Graph ID of the canvas document the node belongs to.
    QString GraphId;
    /// RayTracingPipeline or scene ComputePipeline node ID; empty captures every ray pass.
    QString NodeId;
};

/// Plan pass of a canvas node, by its expanded pass name (`<graph>/<node>`, or any subgraph
/// instance ending in `/<node>` when the document is not the root graph).
/// @return The pass index, or -1 when no pass is named after the node.
[[nodiscard]] foundation::int32 FindRayPathPass(const QStringList& passNames, const RayPathTarget& target);

/// Owner-thread run of one capture on the viewport device. Advance between frames, never while
/// another producer's frame is open; it never waits for the GPU. Reset before the device is destroyed.
class RayPathCapture final
{
public:
    RayPathCapture();
    ~RayPathCapture();
    RayPathCapture(const RayPathCapture&) = delete;
    RayPathCapture& operator=(const RayPathCapture&) = delete;

    /// Mark that the capture variant is being compiled; clears the previous results.
    void BeginBuild();
    /// Report a capture that cannot run; clears the previous results.
    void SetUnavailable(const QString& reason);
    /// Report a failed build or setup; previous results are cleared.
    void Fail(const QString& reason);
    /// Decode the diagnostic package and begin preparing its graph with the frozen inputs.
    /// @param device Viewport device; outlives this run until Reset.
    /// @param surface Headless surface of that device, used for the capture graph's frames.
    /// @param package Diagnostic `.lurgrd` bytes, copied during the call.
    /// @param extraction Frozen World extraction, copied (its geometry storage is shared).
    /// @param freeze Frozen camera and identities; the package must match its interface identity
    /// and the frozen plan's pass count.
    /// @param pixel Normalized position in the exported output, [0, 1) with the top row at zero;
    /// mapped to a launch pixel of the traced ray pass.
    /// @param target Node whose pass is traced and whose color extent the pixel maps into; a whole
    /// graph target maps into the first ray pass. A node that names no ray pass fails the run.
    /// @param sampleCount Progressive samples to record, 1..MAX_RAY_PATH_SAMPLES.
    /// @param compositeVertex Installed renderer_composite recipe; static storage.
    /// @param compositeFragment Installed renderer_composite recipe; static storage.
    /// @return False with Failed or Unavailable and a message when the run cannot begin.
    [[nodiscard]] bool Start(graphics::rhi::DeviceHandle device,
                             graphics::rhi::SurfaceHandle surface,
                             const QByteArray& package,
                             const RenderGraphExtraction& extraction,
                             const RayPathFreeze& freeze,
                             foundation::math::Vector2 pixel,
                             const RayPathTarget& target,
                             foundation::uint32 sampleCount,
                             const graphics::rhi::RasterShaderDescription& compositeVertex,
                             const graphics::rhi::RasterShaderDescription& compositeFragment);
    /// Advance preparation, one sample frame, completion polling or the readback. Owner thread,
    /// between frames; returns without work while the device has an open frame.
    /// @return True when the state or message changed.
    [[nodiscard]] bool Advance() noexcept;
    /// Whether Advance has work to do (Preparing, Tracing or Reading).
    [[nodiscard]] bool HasPendingWork() const noexcept;
    /// Release the diagnostic graph and package; keeps decoded results and state, unless they
    /// were still pending, which become Failed with reason.
    void Release(const QString& reason) noexcept;
    /// Release everything and return to Idle.
    void Reset() noexcept;

    [[nodiscard]] RayPathCaptureState GetState() const noexcept;
    /// Progress or failure text; empty when Ready or Idle.
    [[nodiscard]] const QString& GetMessage() const noexcept;
    /// Decoded events, sorted by pixel, sample, plan pass and sequence; empty before Ready.
    [[nodiscard]] const QVector<graphics::renderer::RayPathEvent>& GetEvents() const noexcept;
    /// Decoded per-invocation summaries; empty before Ready.
    [[nodiscard]] const QVector<graphics::renderer::RayPathSummary>& GetSummaries() const noexcept;
    /// Counters of the decoded capture.
    [[nodiscard]] const graphics::renderer::RayPathCaptureReport& GetReport() const noexcept;
    /// Authored pass names of the capture plan, indexed like RayPathEvent::Pass; empty strings
    /// for passes without names.
    [[nodiscard]] const QStringList& GetPassNames() const noexcept;
    /// Plan pass of the target node; -1 for a whole graph target or before Start resolved it.
    [[nodiscard]] foundation::int32 GetTargetPass() const noexcept;
    /// Launch pixel and extent of the traced pass; zero before preparation selected them.
    [[nodiscard]] foundation::uint32 GetPixelX() const noexcept;
    [[nodiscard]] foundation::uint32 GetPixelY() const noexcept;
    [[nodiscard]] QSize GetLaunchExtent() const noexcept;
    /// Requested and completed sample frames.
    [[nodiscard]] foundation::uint32 GetSampleCount() const noexcept;
    [[nodiscard]] foundation::uint32 GetRenderedSamples() const noexcept;
    /// World object ID of a hit's RenderGraphItem::SourceId in the frozen extraction; empty when unknown.
    [[nodiscard]] QString GetObjectId(foundation::uint64 sourceId) const;

private:
    struct State;
    void fail(const QString& reason) noexcept;
    [[nodiscard]] bool prepare() noexcept;
    [[nodiscard]] bool trace() noexcept;
    [[nodiscard]] bool read() noexcept;
    foundation::UniquePtr<State> mState;
};
} // namespace ludus::editor
