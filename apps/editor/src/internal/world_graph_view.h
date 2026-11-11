#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/preview_shading.h"
#include "internal/viewport_trace.h"

#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QLabel;
class QAction;

namespace ludus::editor
{
class ViewportObjectInteraction;
class RendererWorkspace;
class RenderGraphPreview;
class RenderPreview;
/// Width @p comboBox needs to show @p text whole beside its arrow, measured as the style lays it out.
[[nodiscard]] foundation::int32 ComputeComboBoxWidth(const QComboBox& comboBox, const QString& text);
/// Keep @p inoutComboBox at least as wide as its current choice, so a crowded toolbar moves it
/// behind the overflow chevron whole instead of squeezing its text away. Follows later choices.
void FitComboBoxToCurrentText(QComboBox& inoutComboBox);
/// Viewport toolbar controls a World graph view contributes, placed in different groups.
struct WorldGraphControls
{
    /// Configuration and output choice; they decide what the viewport shows, so they lead the row.
    QWidget* Output = nullptr;
    /// Exposure adjustment; it joins the display group and overflows before the output choice.
    QAction* Exposure = nullptr;
};
/// Interactive World output of the project's active graph. Owns only display/target selection;
/// RendererWorkspace owns builds and RenderGraphPreview owns GPU production and readback.
class WorldGraphView final : public QWidget
{
    Q_OBJECT
public:
    /// Native graph preview must exist; parent owns this display and its controls.
    WorldGraphView(RendererWorkspace& inoutWorkspace, QWidget* parent);
    /// Move the graph controls into the viewport toolbar once.
    [[nodiscard]] WorldGraphControls TakeControls(QWidget* parent, RenderPreview& inoutSource);
    void SetObjectInteraction(ViewportObjectInteraction& inoutInteraction);
    /// Hold the last image while a candidate import waits for catalog publication.
    void SetCanPresent(bool canPresent);
    /// Explicitly clear this view, as when closing its World.
    void ClearFrame();
    /// Show live output statistics. The owning viewport persists this choice with navigation.
    void SetStatisticsVisible(bool isVisible);
    /// Text currently painted over the graph; empty when the overlay is disabled.
    [[nodiscard]] QString GetStatisticsText() const;
    /// Last CPU fallback frame, retained during rebuilding and errors; direct GPU presentation has no QImage.
    [[nodiscard]] const QImage& GetImage() const noexcept;
    /// Number of CPU fallback images received; PreviewViewport tracks directly presented GPU frames.
    [[nodiscard]] foundation::uint64 GetPresentedFrames() const noexcept;
    /// Built-in debug view chosen in the output menu instead of a graph image (see
    /// IsPreviewGraphDebugView). The owning viewport presents its own raster frames in that shading.
    /// @param outShading Receives the chosen mode; preserved when false.
    /// @return False while the menu selects the graph's final output or one of its attachments.
    [[nodiscard]] bool TryGetDebugView(PreviewShading& outShading) const;
    /// Return the output menu to the graph's final output because the chosen built-in debug view
    /// cannot be drawn. The reason stays in the status text until another output is chosen, so the
    /// graph's last image remains on screen instead of an empty viewport. No-op without a debug view.
    void RejectDebugView(const QString& reason);
    /// Explanation of the most recently rejected debug view; empty after another output is chosen.
    [[nodiscard]] LUDUS_INLINE const QString& GetDebugViewProblem() const noexcept
    {
        return mDebugViewProblem;
    }

    /// True while the status text is the short build-failure chip; the full problem is in the Error List.
    [[nodiscard]] LUDUS_INLINE bool HasProblemChip() const noexcept
    {
        return mHasProblemChip;
    }
    /// Words that end the chip's text and name where its full problem is listed.
    [[nodiscard]] static QString GetProblemChipLink();

Q_SIGNALS:
    /// Refreshed overlay text, also consumed by the shared-device presentation.
    void StatisticsChanged();
    /// Current authoring/progress diagnostic, shared with the GPU viewport overlay.
    void StatusChanged(const QString& text);
    /// The output menu switched to, from or between built-in debug views; read TryGetDebugView.
    void DebugViewChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    ViewportObjectInteraction* mInteractionOrNull = nullptr;
    void refreshTargets();
    void refreshFrame();
    void refreshStatus();
    void refreshStatistics();
    bool mHasProblemChip = false;
    RendererWorkspace& mWorkspace;
    RenderGraphPreview& mPreview;
    QComboBox* mTargets;
    QLabel* mStatus;
    QLabel* mStatistics;
    QTimer mStatisticsTimer;
    bool mIsStatisticsVisible = false;
    QImage mImage;
    ViewportPaintTrace mPaintTrace;
    foundation::uint64 mPresentedFrames = 0;
    bool mCanPresent = true;
    QString mDebugViewProblem;
};
} // namespace ludus::editor
