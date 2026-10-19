#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/texture_inspection.h"

#include <QImage>
#include <QPointer>
#include <QString>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;

namespace ludus::editor
{
class RenderGraphPreview;
class TextureCanvas;

// Renderer tab texture viewer: lists every texture of the previewed generation (targets,
// depth and intermediate attachments, not only exports), reads the selected one back through
// RenderGraphPreview without stalling, and shows it with channel, range, exposure, zoom, pan
// and a pixel probe. Selection is by resource name, so it survives Live Preview rebuilds.
// Native GPU preview builds only. See docs/development/render-graph-authoring.md#texture-viewer.
class TextureViewer final : public QWidget
{
    Q_OBJECT
public:
    explicit TextureViewer(QWidget* parentOrNull = nullptr);

    /// Follow a preview's generations and readbacks; null detaches.
    void SetPreview(RenderGraphPreview* previewOrNull);
    /// Select a texture by expanded resource name; an unknown name is kept for later generations.
    void SelectTarget(const QString& name);
    /// Select the first texture a canvas node resolves to (see ResolveCanvasTargets).
    /// @return False when the node touches no texture of the active generation.
    bool SelectCanvasNode(const QString& graphId, const QString& nodeId);
    /// Pin the probe to a texel, as a click does.
    void ProbeTexel(foundation::uint32 x, foundation::uint32 y);
    /// Replace the display controls, as the widgets do.
    void SetDisplaySettings(const TextureDisplaySettings& settings);

    [[nodiscard]] const QString& GetSelectedTarget() const noexcept
    {
        return mSelected;
    }
    [[nodiscard]] TextureDisplaySettings GetDisplaySettings() const;
    /// Last composed display image; null while nothing was read.
    [[nodiscard]] const QImage& GetDisplayImage() const noexcept
    {
        return mDisplay;
    }
    [[nodiscard]] QString GetInfoText() const;
    [[nodiscard]] QString GetProbeText() const;
    [[nodiscard]] QString GetStatusText() const;

private:
    void refreshTargets();
    void refreshTexture();
    void refreshDisplay();
    void refreshControls();
    void showProbe(foundation::uint32 x, foundation::uint32 y, bool isPinned);

    QPointer<RenderGraphPreview> mPreview;
    QListWidget* mTargets;
    QComboBox* mChannel;
    QComboBox* mRange;
    QDoubleSpinBox* mMinimum;
    QDoubleSpinBox* mMaximum;
    QDoubleSpinBox* mExposure;
    QLabel* mExposureLabel;
    QLabel* mInfo;
    QLabel* mProbe;
    QLabel* mStatus;
    TextureCanvas* mCanvas;
    QString mSelected;
    InspectedTexture mTexture;
    QImage mDisplay;
    TextureValueRange mAppliedRange;
    bool mIsUpdatingControls = false;
    bool mHasPinnedProbe = false;
    foundation::uint32 mPinnedX = 0;
    foundation::uint32 mPinnedY = 0;
};

/// Nearest-neighbour zoom and pan view of the display image. Wheel zooms about the cursor,
/// drag pans, double-click fits, hover and click report the texel under the cursor.
class TextureCanvas final : public QWidget
{
    Q_OBJECT
public:
    explicit TextureCanvas(QWidget* parentOrNull = nullptr);
    /// Show an image; a different extent refits the view.
    void SetImage(const QImage& image);
    void Fit();
    void SetTransform(const TextureViewTransform& transform);
    [[nodiscard]] const TextureViewTransform& GetTransform() const noexcept
    {
        return mTransform;
    }

Q_SIGNALS:
    void TexelHovered(foundation::uint32 x, foundation::uint32 y);
    void TexelClicked(foundation::uint32 x, foundation::uint32 y);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QImage mImage;
    TextureViewTransform mTransform;
    QPointF mDragOrigin;
    QPointF mPressPosition;
    bool mIsDragging = false;
    bool mIsFitted = true;
};
} // namespace ludus::editor
