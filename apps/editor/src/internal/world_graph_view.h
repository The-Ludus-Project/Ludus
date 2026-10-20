#pragma once

#include <ludus/foundation/base/types.h>

#include <QImage>
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
    /// Last complete graph frame, retained during rebuilding and errors.
    [[nodiscard]] const QImage& GetImage() const noexcept;
    /// Number of complete images received by this display.
    [[nodiscard]] foundation::uint64 GetPresentedFrames() const noexcept;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void refreshTargets();
    void refreshFrame();
    void refreshStatus();
    RendererWorkspace& mWorkspace;
    RenderGraphPreview& mPreview;
    QComboBox* mTargets;
    QLabel* mStatus;
    QImage mImage;
    foundation::uint64 mPresentedFrames = 0;
    bool mCanPresent = true;
};
} // namespace ludus::editor
