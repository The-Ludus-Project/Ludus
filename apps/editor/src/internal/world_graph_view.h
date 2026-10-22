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
    /// Hold the last image while a candidate import waits for catalog publication.
    void SetCanPresent(bool canPresent);
    /// Explicitly clear this view, as when closing its World.
    void ClearFrame();
    /// Show live output statistics. The owning viewport persists this choice with navigation.
    void SetStatisticsVisible(bool isVisible);
    /// Text currently painted over the graph; empty when the overlay is disabled.
    [[nodiscard]] QString GetStatisticsText() const;
    /// Last complete graph frame, retained during rebuilding and errors.
    [[nodiscard]] const QImage& GetImage() const noexcept;
    /// Number of complete images received by this display.
    [[nodiscard]] foundation::uint64 GetPresentedFrames() const noexcept;

Q_SIGNALS:
    /// Refreshed overlay text, also consumed by the shared-device presentation.
    void StatisticsChanged();
    /// Current authoring/progress diagnostic, shared with the GPU viewport overlay.
    void StatusChanged(const QString& text);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void refreshTargets();
    void refreshFrame();
    void refreshStatus();
    void refreshStatistics();
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
