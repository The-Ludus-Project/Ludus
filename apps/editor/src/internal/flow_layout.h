#pragma once

#include <ludus/foundation/base/types.h>

#include <QLayout>
#include <QList>

namespace ludus::editor
{
/// Lays its items out in one row at their size hints while they fit. When they do not, they wrap into a grid of
/// equal cells as wide as the widest item, stretched to fill each row, so a row of buttons in a narrow dock grows
/// taller instead of clipping a label ("Import WAV / FL") and the wrapped buttons line up instead of leaving a
/// ragged edge. A row narrower than the widest item gives every item the whole row. Hidden items take no space.
/// GUI thread only.
class FlowLayout final : public QLayout
{
public:
    /// spacingPixels separates neighbouring items across and between rows.
    FlowLayout(QWidget* parentOrNull, foundation::int32 spacingPixels);
    ~FlowLayout() override;
    FlowLayout(const FlowLayout&) = delete;
    FlowLayout& operator=(const FlowLayout&) = delete;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    [[nodiscard]] QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rectangle) override;
    /// Every item in one row.
    [[nodiscard]] QSize sizeHint() const override;
    /// The widest single item: the narrowest the row can be without clipping one.
    [[nodiscard]] QSize minimumSize() const override;

private:
    /// Places the items in area (when shouldPlace) and returns the height they need.
    [[nodiscard]] foundation::int32 arrange(const QRect& area, bool shouldPlace) const;

    QList<QLayoutItem*> mItems;
};
} // namespace ludus::editor
