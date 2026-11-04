#pragma once

#include <ludus/foundation/base/types.h>

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QWidget>

namespace ludus::editor
{
class RenderGraphCanvas;

/// One card as the minimap draws it: where it sits in the scene, its domain colour and whether it is selected.
struct RenderGraphMinimapCard final
{
    QRectF Bounds;
    QColor Colour;
    bool IsSelected = false;
};

/// Uniform scale and offset between scene units and the minimap's pixels, for one set of content bounds.
/// A value type so the mapping is testable without a widget.
struct RenderGraphMinimapProjection final
{
    QRectF Content;                ///< Scene rectangle the map shows.
    QRectF Target;                 ///< Widget rectangle the content is fitted into, centred.
    foundation::float64 Scale = 1; ///< Widget pixels per scene unit.
    QPointF Origin;                ///< Widget position of Content's top left.

    [[nodiscard]] QPointF ToMap(QPointF scenePoint) const;
    [[nodiscard]] QPointF ToScene(QPointF mapPoint) const;
    [[nodiscard]] QRectF ToMap(const QRectF& sceneRectangle) const;
};

/// Fit content into target keeping the aspect ratio. Empty or non-finite input gives scale 1 at target's centre.
[[nodiscard]] RenderGraphMinimapProjection MakeRenderGraphMinimapProjection(const QRectF& content,
                                                                            const QRectF& target);

/// Overview of the whole graph pinned over the canvas corner: every card as a small block, the part of the
/// graph in view as an outlined window. Pressing or dragging inside it moves the canvas view there. It
/// only reads the canvas and moves its view; it never edits the document. GUI thread only.
class RenderGraphMinimap final : public QWidget
{
    Q_OBJECT
public:
    /// The minimap is a child of the canvas viewport and must not outlive the canvas.
    explicit RenderGraphMinimap(RenderGraphCanvas& inoutCanvas, QWidget* parentOrNull);
    /// Pixel size at the design scale, for placing it in a corner.
    [[nodiscard]] QSize sizeHint() const override;
    /// Current mapping between scene units and this widget's pixels.
    [[nodiscard]] RenderGraphMinimapProjection GetProjection() const;
    /// Scene point under a widget position; clamped to the content shown.
    [[nodiscard]] QPointF GetScenePosition(QPoint widgetPosition) const;
    /// Whether the view is being moved by dragging the map.
    [[nodiscard]] bool IsDragging() const noexcept
    {
        return mIsDragging;
    }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void centreViewAt(QPoint widgetPosition);

    RenderGraphCanvas& mCanvas;
    RenderGraphMinimapProjection mDragProjection;
    bool mIsDragging = false;
};
} // namespace ludus::editor
