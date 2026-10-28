#pragma once

#include <QLabel>

QT_BEGIN_NAMESPACE
class QPainter;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderPreview;

/// Camera-driven editor reference grid while no scene snapshot is available.
/// Borrows the viewport's preview; never admits a GPU device or creates World objects.
class EmptyWorldView final : public QLabel
{
public:
    /// The borrowed preview must outlive this widget.
    EmptyWorldView(RenderPreview& inoutPreview, QWidget* parentOrNull);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// Draws the label text over the viewport background in the fixed caption colour.
    void drawCaption(QPainter& inoutPainter) const;
    RenderPreview& mPreview;
};
} // namespace ludus::editor
