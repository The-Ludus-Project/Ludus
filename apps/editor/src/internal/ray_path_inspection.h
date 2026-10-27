#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>

#include "internal/ray_path_capture.h"
#include "internal/ray_path_overlay.h"

#include <QObject>
#include <QPointer>
#include <QString>

namespace ludus::editor
{
class RenderGraphPreview;

/// Where the author is in the ray path inspection workflow.
enum class RayPathInspectionStage : foundation::uint8
{
    Inactive,     ///< The World viewport shows the live graph.
    PickingPixel, ///< The next click on the graph output chooses the traced pixel.
    Inspecting,   ///< The graph is frozen; the viewport shows the raster scene and the frozen paths.
};

/// Editor workflow of ray path inspection ("Editor workflow" in
/// docs/architecture/render-graph-authoring.md#ray-path-inspection-proposed): choose a node or the
/// whole graph, pick a pixel, freeze the preview, have the capture variant built, trace it and
/// explore the frozen paths. Owns only the workflow and presentation settings; RenderGraphPreview
/// owns the freeze and capture, the workspace owns the build job and PreviewViewport presentation.
/// GUI thread only.
class RayPathInspection final : public QObject
{
    Q_OBJECT
public:
    /// Samples a capture records; the first slice traces one pixel at 16 samples.
    static constexpr foundation::uint32 SAMPLE_COUNT = graphics::renderer::DEFAULT_RAY_PATH_SAMPLES;

    explicit RayPathInspection(QObject* parentOrNull = nullptr);
    /// Borrow the live preview whose generation is captured; null detaches. Leaves any inspection.
    void SetPreview(RenderGraphPreview* previewOrNull);
    /// Start choosing a pixel for target. When capture is unavailable (no attached device, a raster
    /// or software renderer), nothing freezes and the capture reports Unavailable with the reason.
    /// @return True when the next PickPixel freezes the preview.
    [[nodiscard]] bool Begin(const RayPathTarget& target);
    /// Freeze the preview at the pixel chosen in its exported output and request the build.
    /// @param pixel Normalized output position, [0, 1), top row zero.
    /// @return False when not picking or the preview could not freeze.
    [[nodiscard]] bool PickPixel(foundation::math::Vector2 pixel);
    /// Trace the compiled capture variant at packagePath (the build job's diagnostic package).
    void AcceptBuild(const QString& packagePath);
    /// Report that the capture variant could not be built.
    void RejectBuild(const QString& reason);
    /// Return to the live graph from the current editor camera; also cancels picking.
    void Leave();

    [[nodiscard]] RayPathInspectionStage GetStage() const noexcept;
    [[nodiscard]] const RayPathTarget& GetTarget() const noexcept;
    /// The borrowed preview, or null.
    [[nodiscard]] RenderGraphPreview* GetPreviewOrNull() const noexcept;
    /// One-line state for the inspector and viewport: picking, building, tracing, ready, stale,
    /// truncated, failed or unavailable.
    [[nodiscard]] QString GetStatus() const;

    void SetXRay(bool isXRay);
    [[nodiscard]] bool IsXRay() const noexcept;
    void SetColorMode(RayPathColorMode mode);
    [[nodiscard]] RayPathColorMode GetColorMode() const noexcept;
    /// Draw only one progressive sample; negative draws every sample.
    void SetSampleFilter(foundation::int32 sample);
    [[nodiscard]] foundation::int32 GetSampleFilter() const noexcept;
    /// Highlight one decoded event by index; negative clears. A hit selects its World object.
    void SelectEvent(foundation::int32 event);
    [[nodiscard]] foundation::int32 GetSelectedEvent() const noexcept;
    /// World lines of the decoded capture with the current settings.
    /// @param sceneBoundsOrNull World bounds misses are drawn to; null uses a fixed miss length.
    [[nodiscard]] RayPathOverlay BuildOverlay(const foundation::math::Aabb3* sceneBoundsOrNull) const;

Q_SIGNALS:
    /// Stage, capture state, staleness or status changed.
    void Changed();
    /// What the overlay draws changed (capture results or presentation settings).
    void OverlayChanged();
    /// The preview froze; build the capture variant of GetPreviewOrNull()->GetRayPathFreeze().
    void BuildRequested();
    /// A selected hit names this World object (its extraction SourceId).
    void ObjectSelectionRequested(const QString& objectId);

private:
    void setStage(RayPathInspectionStage stage);
    QPointer<RenderGraphPreview> mPreview;
    RayPathInspectionStage mStage = RayPathInspectionStage::Inactive;
    RayPathTarget mTarget;
    foundation::math::Vector2 mPixel{};
    bool mIsXRay = false;
    RayPathColorMode mColorMode = RayPathColorMode::Kind;
    foundation::int32 mSampleFilter = -1;
    foundation::int32 mSelectedEvent = -1;
    QMetaObject::Connection mCaptureConnection;
};
} // namespace ludus::editor
