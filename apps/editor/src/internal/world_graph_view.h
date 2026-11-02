#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/viewport_trace.h"

#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QLabel;

namespace ludus::editor
{
class ViewportObjectInteraction;
class RendererWorkspace;
class RenderGraphPreview;
/// Interactive World output of the project's active graph. Owns only display/target selection;
/// RendererWorkspace owns builds and RenderGraphPreview owns GPU production and readback.
class WorldGraphView final : public QWidget
{
    Q_OBJECT
public:
    /// Native graph preview must exist; parent owns this display and its controls.
    WorldGraphView(RendererWorkspace& inoutWorkspace, QWidget* parent);
    /// Move the output/configuration controls into the viewport toolbar once.
    QWidget* TakeControls(QWidget* parent);
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

    /// True while the status text is the short build-failure chip; the full problem is in the Error List.
    [[nodiscard]] bool HasProblemChip() const noexcept
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
};
} // namespace ludus::editor
