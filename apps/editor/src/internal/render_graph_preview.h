#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/raster.h>

#include "internal/graph_display_frames.h"
#include "internal/render_graph_targets.h"
#include "internal/render_scene.h"
#include "internal/texture_inspection.h"
#include "internal/viewport_statistics.h"
#include "internal/viewport_trace.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>

namespace ludus::editor
{
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

/// Result of an explicit request to preview a cooked runtime generation.
enum class RenderGraphPreviewLoad : foundation::uint8
{
    Staged,          ///< The generation is preparing beside the drawing one.
    Waiting,         ///< Decoded; it stages once a viewport device is attached.
    Unreadable,      ///< The `.lurgrt` file is missing, unreadable or larger than the runtime bound.
    Malformed,       ///< RenderGraphPackage::Decode rejected the bytes.
    Unsupported,     ///< Decoded, but the plan has no exported color result the editor can show, or it
                     ///< needs an optional feature (OIDN denoising) this editor build lacks.
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
    /// No fallback is needed for a raster request. Selection is repeated on device attachment.
    [[nodiscard]] RenderGraphPreviewLoad Load(const QString& runtimePackagePath,
                                              const QString& rasterFallbackPath = {});
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

    /// Textures of the active generation; empty before one is active.
    [[nodiscard]] const RenderGraphTargetCatalogue& GetTargetCatalogue() const noexcept;
    /// Choose the texture the viewer reads back, by expanded resource name; empty stops reading.
    /// The name is resolved against each newly active generation, so a selection survives
    /// rebuilds while the texture still exists. The texture is read after every presented
    /// frame (and once immediately when a frame is already shown), never while a frame or the
    /// export copy is in flight, and without waiting for the GPU.
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
    /// Human-readable state, distinguishing the shown frame from a pending or failed candidate.
    [[nodiscard]] QString GetStatus() const;
    /// World-facing progress or error text; empty after a successful frame.
    [[nodiscard]] QString GetDisplayStatus() const;

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

private:
    struct State;
    [[nodiscard]] RenderGraphPreviewLoad stage();
    void tick();
    void advance();
    void finishCopy();
    void finishDisplayCopy();
    void publishFrame(bool isGpuFrame = false);
    [[nodiscard]] bool beginInspection();
    void finishInspection();
    void setStatus(const QString& text);

    foundation::UniquePtr<State> mState;
    QImage mImage;
    foundation::uint64 mPresentedGeneration = 0;
    foundation::uint64 mPresentedRevision = 0;
    foundation::uint64 mPresentedSession = 0;
    QString mStatus;
    InspectedTexture mInspected;
    QString mInspectionStatus;
    foundation::uint64 mInspectedGeneration = 0;
    foundation::uint64 mInspectedRevision = 0;
};
} // namespace ludus::editor
