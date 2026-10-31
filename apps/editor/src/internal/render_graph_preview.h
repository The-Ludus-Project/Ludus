#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>

#include "internal/graph_display_frames.h"
#include "internal/ray_path_capture.h"
#include "internal/render_graph_targets.h"
#include "internal/render_scene.h"
#include "internal/texture_inspection.h"
#include "internal/viewport_statistics.h"
#include "internal/viewport_trace.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>

namespace ludus::editor
{
/// Name of an RHI raster status as diagnostics show it, for example "CapacityExceeded".
[[nodiscard]] QString DescribeRasterStatus(graphics::rhi::RasterStatus status);
/// Describe a rejected extraction, including measured memory admission and preservation of the last frame.
[[nodiscard]] QString
DescribeRenderGraphExtractionFailure(graphics::rhi::RasterStatus status,
                                     const graphics::renderer::SceneShaderDiagnostics& diagnostics);
/// One World extraction converted to RenderGraph records. Mesh geometry borrows Scene's
/// storage (its PreviewMesh arrays or its shared cooked Definition), which this value keeps
/// alive by implicit sharing and reference counting until it is replaced.
struct RenderGraphExtraction final
{
    RenderScene Scene;
    QVector<graphics::renderer::RenderGraphMesh> Meshes;
    QVector<graphics::renderer::RenderGraphItem> Items;
    QVector<graphics::renderer::SceneShaderMaterial> Materials;
    QVector<graphics::renderer::DirectLight> Lights;
    QVector<QImage> MaterialImages;
    QVector<foundation::uint32> MaterialOverrides;
    graphics::renderer::SceneShaderInputs ShaderInputs;
    graphics::renderer::ViewDescription View{};
    /// Nonzero caller revision; a changed revision makes the preview prepare again.
    foundation::uint64 Revision = 0;
    /// Nonzero identity changed with every non-camera input; zero disables scene reuse.
    foundation::uint64 ContentRevision = 0;
};

/// Convert the World extraction used by the editor viewport into RenderGraph inputs: one static
/// mesh per scene mesh and one item per object with its straight RGBA tint (object tint times
/// surface tint). Translucent tints are declared Transparent, never silently made opaque.
/// A cooked scene (imported or merged World, Definition set) keeps its geometry only in the
/// decoded definition; its meshes are borrowed from there without copying. Otherwise the
/// authored PreviewMesh arrays are borrowed.
/// @param view Must use the conventional depth convention the runtime admits; see
/// RenderPreview::DescribeConventionalView.
/// @return False with outError when the scene exceeds the runtime format bounds, is empty or
/// its definition does not parallel its mesh table; outExtraction is unchanged on failure.
[[nodiscard]] bool MakeRenderGraphExtraction(const RenderScene& scene,
                                             const graphics::renderer::ViewDescription& view,
                                             foundation::uint64 revision,
                                             RenderGraphExtraction& outExtraction,
                                             QString& outError);

/// Optional authoring provenance for a cooked package. The preview admits it only when
/// Generation and RuntimeIdentity match the decoded build generation and exact package bytes.
/// InputIdentity is the editor semantic source/configuration digest; empty means unverified.
struct RenderGraphPreviewSource final
{
    QByteArray InputIdentity;
    QString Configuration;
    QByteArray Generation;                 ///< Hex build-generation identity, as reported by the cooker.
    QByteArray RuntimeIdentity{};          ///< Hex SHA-256 of the cooked runtime bytes, binding the configuration too.
    QString RootFile{};                    ///< Package-relative root graph that was cooked.
    foundation::uint64 SourceRevision = 0; ///< Document revision captured by the build; 0 when unverified.
};

/// Result of an explicit request to preview a cooked runtime generation.
enum class RenderGraphPreviewLoad : foundation::uint8
{
    Staged,          ///< The generation is preparing beside the drawing one.
    Waiting,         ///< Decoded; it stages once a viewport device is attached.
    Unreadable,      ///< The `.lurgrt` file is missing, unreadable or larger than the runtime bound.
    Malformed,       ///< RenderGraphPackage::Decode rejected the bytes.
    Unsupported,     ///< Decoded, but the plan has no exported color result the editor can show.
    RestartRequired, ///< The CPU interface differs from this editor's compiled interface.
    Failed,          ///< Staging failed on the device; the drawing generation is unchanged.
};

// RG3b trusted in-editor preview of a cooked render-graph generation. It owns one
// RenderGraphActivation on the existing editor viewport device (borrowed from the World
// RenderPreview), feeds it the World extraction, renders the active generation's exported
// color into retained GPU display snapshots (or an explicit CPU fallback). The presented image changes
// only when a complete copy of an active generation's drawable frame arrives: a pending,
// failed, coverage-rejected or restart-requiring candidate, a lost device or a failed copy keep
// the last drawable view. Export consumers (the copy) always finish before a Poll that may
// activate or a new Render. Owner (GUI) thread only; call Detach before the device is destroyed.
// See docs/development/render-graph-authoring.md#node-canvas-and-preview-rg3b.
class RenderGraphPreview final : public QObject
{
    Q_OBJECT
public:
    /// Maximum `.lurgrt` size read from disk; matches the runtime format's 16 MiB bound.
    static constexpr qsizetype MAX_PACKAGE_BYTES = qsizetype{16} * 1024 * 1024;

    /// @param interfaceIdentity IDENTITY_BYTES digest of the editor's compiled CPU interface; copied.
    /// @param compositeVertex Installed renderer_composite recipe; static storage outliving this object.
    /// @param compositeFragment Installed renderer_composite recipe; same lifetime.
    RenderGraphPreview(foundation::BorrowedView<const foundation::uint8> interfaceIdentity,
                       const graphics::rhi::RasterShaderDescription& compositeVertex,
                       const graphics::rhi::RasterShaderDescription& compositeFragment,
                       QObject* parentOrNull = nullptr);
    ~RenderGraphPreview() override;
    RenderGraphPreview(const RenderGraphPreview&) = delete;
    RenderGraphPreview& operator=(const RenderGraphPreview&) = delete;

    /// Bind the viewport device and its (headless) surface; stages a waiting generation.
    /// @return False when already attached or activation initialization fails.
    [[nodiscard]] bool Attach(graphics::rhi::DeviceHandle device, graphics::rhi::SurfaceHandle surface);
    /// Release every generation and in-flight copy before the device is destroyed. The last
    /// presented image stays visible and is reported as stale.
    void Detach() noexcept;
    [[nodiscard]] bool IsAttached() const noexcept;
    /// Explicitly preview one cooked runtime generation; the previous request is superseded.
    /// An RT request automatically selects rasterFallbackPath when native traversal is unavailable
    /// or Force raster is selected. The fallback must be an authored raster-only package.
    /// Optional source provenance is copied and accepted only for its exact decoded generation.
    /// No fallback is needed for a raster request. Selection is repeated on device attachment.
    [[nodiscard]] RenderGraphPreviewLoad Load(const QString& runtimePackagePath,
                                              const QString& rasterFallbackPath = {},
                                              const RenderGraphPreviewSource& source = {});
    /// Queue selection between frames; retain the last drawable image while preparing.
    void SetForceRaster(bool shouldForceRaster);
    [[nodiscard]] bool ShouldForceRaster() const noexcept;
    /// Choose the World display attachment by expanded name; empty selects the exported output.
    /// Missing targets retain the last image and report a diagnostic. Applies after producer completion.
    void SetOutputTarget(const QString& name);
    /// True while a completed graph attachment is being copied for display.
    [[nodiscard]] bool IsCopyingOutput() const noexcept;
    /// Replace the World extraction. A new revision is prepared by the active graph and candidate.
    void SetExtraction(const RenderGraphExtraction& extraction);
    /// Replace only the camera and publication revision of the retained extraction.
    void SetCamera(const graphics::renderer::ViewDescription& view, foundation::uint64 revision);
    /// Queue the owning viewport's drawable size (physical pixels before rendering scale).
    /// Applied between output consumers; zero suspends new submissions and keeps the image.
    void SetReferenceSize(const QSize& size);
    /// Export extent of the generation the next frame will show (candidate first), or 0x0.
    [[nodiscard]] QSize GetTargetSize() const noexcept;
    /// Advance activation, preparation, rendering and the asynchronous copy without waiting.
    /// @param preparationMilliseconds Active caller work converting the current World/camera, in ms.
    void Tick(foundation::float64 preparationMilliseconds = 0);
    /// GUI-thread observer for frame stages and image-to-paint correlation.
    [[nodiscard]] ViewportFrameTrace& GetFrameTrace() noexcept;
    /// Completed output statistics; retained while preparation is pending or fails, reset on Detach.
    [[nodiscard]] const GraphViewportStatistics& GetStatistics() const noexcept;
    /// GUI-thread delivery of an accepted conversion. Worker execution is a separate series;
    /// only publicationMilliseconds contributes to owner-thread CPU work. Ignored after Detach.
    void RecordInspectionConversion(foundation::float64 workMilliseconds,
                                    foundation::float64 elapsedMilliseconds,
                                    foundation::float64 publicationMilliseconds) noexcept;

    /// Select shared-device GPU snapshots; CPU readback remains the explicit offscreen fallback.
    void SetDirectPresentation(bool shouldPresentDirectly) noexcept;
    [[nodiscard]] bool HasPendingWork() const noexcept;
    /// True when the last published frame is a GPU snapshot; false for depth/readback display.
    [[nodiscard]] bool HasGpuFrame() const noexcept;
    [[nodiscard]] GraphDisplayFrames& GetDisplayFrames() noexcept;

    /// Last complete CPU fallback frame; direct presentation publishes through GetDisplayFrames.
    [[nodiscard]] const QImage& GetImage() const noexcept
    {
        return mImage;
    }
    /// Raw texels of the output GetImage was converted from; invalid until a CPU fallback frame
    /// is published. Unlike GetImage, HDR outputs keep their linear RGBA16F values.
    [[nodiscard]] const InspectedTexture& GetOutputTexture() const noexcept
    {
        return mOutputTexture;
    }
    /// Runtime package and raster fallback of the latest Load request; empty before one.
    [[nodiscard]] const QString& GetRequestedPackagePath() const noexcept;
    [[nodiscard]] const QString& GetRasterFallbackPath() const noexcept;
    /// Display attachment chosen by SetOutputTarget; empty selects the exported output.
    [[nodiscard]] const QString& GetOutputTarget() const noexcept;
    /// Whether a frame capture could capture one output of the active generation, judged without a device by
    /// FindRenderGraphCaptureProblem when the generation was loaded.
    /// @param resource Logical resource; ABSENT selects the generation's exported output, as the viewport does.
    /// @param outProblem Receives None when the route can be captured, otherwise why not; unchanged on false.
    /// @return False when no generation of GetRequestedPackagePath is active, for example while its first
    /// generation prepares or while the raster fallback is shown instead.
    [[nodiscard]] bool
    FindActiveCaptureProblem(foundation::uint32 resource,
                             graphics::renderer::RenderGraphCaptureProblem& outProblem) const noexcept;
    /// Activation serial whose frame is shown; 0 when nothing was presented.
    [[nodiscard]] foundation::uint64 GetPresentedGeneration() const noexcept
    {
        return mPresentedGeneration;
    }
    /// Extraction revision of the shown frame; 0 when nothing was presented.
    [[nodiscard]] foundation::uint64 GetPresentedRevision() const noexcept
    {
        return mPresentedRevision;
    }
    [[nodiscard]] graphics::renderer::RenderGraphActivationReport GetReport() const noexcept;

    /// Provenance belonging to GetTargetCatalogue's active generation, never a pending candidate.
    /// Empty when no verified build identity was supplied or the selected package differs.
    /// Borrowed until activation/reset; copy it to retain a snapshot.
    [[nodiscard]] const RenderGraphPreviewSource& GetActiveSource() const noexcept;
    /// True when the active catalogue belongs to a completed presented frame in this device
    /// session and drawable extent. False during activation/resize until the new frame presents.
    [[nodiscard]] bool HasPresentedCatalogue() const noexcept;
    /// Textures of the active generation; empty before one is active.
    [[nodiscard]] const RenderGraphTargetCatalogue& GetTargetCatalogue() const noexcept;
    /// Suspend diagnostics while the texture viewer is hidden; cancels unfinished reads.
    /// The selection and last completed snapshot survive. Re-enabling requests a fresh snapshot.
    void SetInspectionEnabled(bool isEnabled) noexcept;
    /// Choose the texture the viewer reads back, by expanded resource name; empty stops reading.
    /// The name is resolved against each newly active generation, so a selection survives
    /// rebuilds while the texture still exists. When enabled, selection requests an immediate
    /// snapshot between frames; automatic refreshes are limited to four per second.
    /// Band readbacks use an independent GPU snapshot and do not hold up later rendering.
    void SetInspectedTarget(const QString& name);
    [[nodiscard]] const QString& GetInspectedTarget() const noexcept;
    /// Last read of the inspected texture; invalid when none completed or the texture was undefined.
    [[nodiscard]] const InspectedTexture& GetInspectedTexture() const noexcept
    {
        return mInspected;
    }
    /// Why the inspected texture is not shown; empty after a successful read.
    [[nodiscard]] const QString& GetInspectionStatus() const noexcept
    {
        return mInspectionStatus;
    }
    /// Activation serial and World revision of the frame GetInspectedTexture came from.
    [[nodiscard]] foundation::uint64 GetInspectedGeneration() const noexcept
    {
        return mInspectedGeneration;
    }
    [[nodiscard]] foundation::uint64 GetInspectedRevision() const noexcept
    {
        return mInspectedRevision;
    }
    /// Why the drawing generation's ray paths cannot be captured here; empty when they can.
    /// Capture needs an attached device, a drawn generation and a ray renderer, with native ray
    /// queries or software (compute) traversal: a raster selection (no ray pass, a GPU/API without
    /// the native queries the graph requires, or Force raster) reports unavailable, while that
    /// renderer keeps drawing and navigating normally.
    [[nodiscard]] QString DescribeRayPathCaptureProblem() const;
    /// Freeze the live graph for ray path inspection. Copies the graph camera, World revision,
    /// content revision (the capture graph's sampling seed), reference and output sizes, generation
    /// serial and identity of the drawing generation. While frozen, SetCamera no longer reaches the
    /// graph camera (the latest camera is kept for ThawRayPaths), the live graph submits no frames,
    /// and SetExtraction, a different reference size or a newly active generation marks the capture
    /// stale; the frozen capture is never mixed with newer inputs.
    /// @return False with outError (DescribeRayPathCaptureProblem) when ray paths cannot be captured.
    [[nodiscard]] bool FreezeRayPaths(QString& outError);
    /// Leave ray path inspection: release the capture and return the live graph to the latest
    /// World extraction and camera supplied while frozen. No effect when not frozen.
    void ThawRayPaths();
    [[nodiscard]] bool IsRayPathFrozen() const noexcept;
    /// Inputs copied by FreezeRayPaths; default values when not frozen.
    [[nodiscard]] const RayPathFreeze& GetRayPathFreeze() const noexcept;
    /// Report that the capture variant of the frozen generation is compiling.
    void BeginRayPathBuild();
    /// Report that compiling the capture variant failed, or that capture is unavailable.
    void FailRayPathCapture(const QString& reason, bool isUnavailable = false);
    /// Trace a compiled diagnostic capture package of the frozen generation in its own graph,
    /// with the frozen extraction and camera; Tick advances it between frames without waiting.
    /// @param packagePath `.lurgrd` written by `render-graph ray-path-capture`, at most MAX_PACKAGE_BYTES.
    /// @param pixel Normalized position in the exported output, [0, 1), top row zero.
    /// @param target Node whose ray pass is traced, or the whole graph.
    /// @param sampleCount Progressive samples to record.
    /// @return False when not frozen or the run cannot begin; GetRayPathCapture reports why.
    [[nodiscard]] bool StartRayPathCapture(const QString& packagePath,
                                           foundation::math::Vector2 pixel,
                                           const RayPathTarget& target,
                                           foundation::uint32 sampleCount);
    /// The capture of the frozen generation and its decoded results.
    [[nodiscard]] const RayPathCapture& GetRayPathCapture() const noexcept;
    /// Why the frozen capture no longer matches the World, viewport or graph; empty while current.
    [[nodiscard]] const QString& GetRayPathStaleReason() const noexcept;
    /// Human-readable state, distinguishing the shown frame from a pending or failed candidate.
    [[nodiscard]] QString GetStatus() const;
    /// World-facing progress or error text; empty after a successful frame. When pixels of a ray pass latched the
    /// graph's failure colour, it names the pass, the pixel count and the main failure reason, after any progress
    /// text (the active graph keeps drawing while its first frame is presented or a replacement prepares).
    [[nodiscard]] QString GetDisplayStatus() const;
    /// Pixels and main reason of the latched failure colour, or empty; part of GetDisplayStatus.
    [[nodiscard]] LUDUS_INLINE const QString& GetFailureText() const noexcept
    {
        return mFailureText;
    }

Q_SIGNALS:
    void PreparingSubmission();
    void SubmissionRecorded();
    /// A new complete GPU snapshot or CPU fallback image replaced the presented frame.
    void FramePresented();
    /// Activation or candidate state changed; the status text may differ.
    void Changed();
    /// A new generation became active; GetTargetCatalogue may list other textures.
    void TargetsChanged();
    /// GetInspectedTexture or GetInspectionStatus changed.
    void TargetInspected();
    /// The freeze, its staleness or the ray path capture's state changed.
    void RayPathCaptureChanged();

private:
    struct State;
    [[nodiscard]] RenderGraphPreviewLoad stage();
    void tick();
    /// Read the failure summaries of the active generation's ray passes in turn and name the main reason.
    void pollFailures();
    void advance();
    void finishCopy();
    void finishDisplayCopy();
    void publishFrame(bool isGpuFrame = false);
    [[nodiscard]] bool beginInspection();
    void finishInspection();
    void setStatus(const QString& text);
    void markRayPathStale(const QString& reason);

    foundation::UniquePtr<State> mState;
    QImage mImage;
    InspectedTexture mOutputTexture;
    foundation::uint64 mPresentedGeneration = 0;
    foundation::uint64 mPresentedRevision = 0;
    foundation::uint64 mPresentedSession = 0;
    QString mStatus;
    /// GetFailureText: the main latched failure of the active generation, or empty.
    QString mFailureText;
    InspectedTexture mInspected;
    QString mInspectionStatus;
    foundation::uint64 mInspectedGeneration = 0;
    foundation::uint64 mInspectedRevision = 0;
};
} // namespace ludus::editor
