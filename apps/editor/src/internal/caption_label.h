#pragma once

#include <QLabel>
#include <QSize>
#include <QString>

QT_BEGIN_NAMESPACE
class QPaintEvent;
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// A one-line caption under a panel's commands. It keeps its line whether or not it shows a hint, so nothing below
/// moves or draws over it, and a hint too long for the panel ends in an ellipsis with the whole text in the tooltip.
/// GUI thread only.
class CaptionLabel final : public QLabel
{
public:
    explicit CaptionLabel(QWidget* parentOrNull);
    /// Show text on the caption line, with the whole text in the tooltip since a long hint is elided.
    void SetCaption(const QString& text);
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};
} // namespace ludus::editor
