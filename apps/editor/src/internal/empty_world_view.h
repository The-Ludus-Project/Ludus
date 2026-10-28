#pragma once

#include <ludus/foundation/base/types.h>

#include <QColor>
#include <QLabel>

QT_BEGIN_NAMESPACE
class QPainter;
QT_END_NAMESPACE

namespace ludus::graphics::renderer
{
struct WorldGridLine;
} // namespace ludus::graphics::renderer

namespace ludus::editor
{
class RenderPreview;

/// Axis a world grid line lies along (0 X, 1 Y, 2 Z), or a negative value for a line off the axes.
[[nodiscard]] foundation::int32 GetAxisIndexOrNegative(const graphics::renderer::WorldGridLine& line) noexcept;
/// Colour of an axis letter: the axis colour mixed toward white until it reads at WCAG text contrast on the viewport.
[[nodiscard]] QColor GetAxisLetterColour(QColor axisColour);

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
